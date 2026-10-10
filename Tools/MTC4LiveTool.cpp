// Developer tool, not shipped.
//
//   MTC4LiveTool snapshot <out.png>   Render the editor to a PNG.
//   MTC4LiveTool monitor [source] [--interval s]
//                                     Live MTC reader for a MIDI source
//                                     (default "Live MTC", every 0.25 s).
//   MTC4LiveTool e2e                  Drive the real processor with a fake
//                                     playhead in real time, receive its
//                                     virtual MIDI port via CoreMIDI, decode
//                                     the MTC and check lock, locate and jitter.

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <CoreMIDI/CoreMIDI.h>

#include <mach/mach_time.h>

#include <atomic>
#include <cmath>
#include <csignal>
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
    double wallMs;      // CoreMIDI timestamp (intended output time), or arrival if unstamped
    double arrivalMs;   // when it reached us
    bool stamped;
    std::vector<uint8_t> bytes;
};

struct Receiver
{
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIEndpointRef connected = 0;
    int matchingSources = 0; // from the last connect()
    std::mutex lock;
    std::vector<Received> events;

    static void readProc (const MIDIPacketList* list, void* refCon, void*)
    {
        auto& self = *static_cast<Receiver*> (refCon);
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto* packet = &list->packet[0];

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

                const bool stamped = packet->timeStamp != 0;
                const auto stampMs = stamped ? (double) packet->timeStamp * machToMs + offsetMs : now;
                self.events.push_back ({ stampMs, now, stamped,
                                         std::vector<uint8_t> (packet->data + pos, packet->data + pos + len) });
                pos += len;
            }

            packet = MIDIPacketNext (packet);
        }
    }

    /** Connects to the source with this name, or with this CoreMIDI unique ID
        if byUniqueId (needed when several ports share a name). */
    bool connect (const juce::String& sourceName, bool byUniqueId = false)
    {
        if (client == 0 && MIDIClientCreate (CFSTR ("MTC4Live tool"), nullptr, nullptr, &client) != noErr)
            return false;

       #pragma clang diagnostic push
       #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (port == 0 && MIDIInputPortCreate (client, CFSTR ("in"), readProc, this, &port) != noErr)
            return false;
       #pragma clang diagnostic pop

        MIDIEndpointRef match = 0;
        matchingSources = 0;

        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
        {
            const auto src = MIDIGetSource (i);
            CFStringRef s = nullptr;
            MIDIObjectGetStringProperty (src, kMIDIPropertyName, &s);
            const auto n = juce::String::fromCFString (s);
            if (s != nullptr) CFRelease (s);

            SInt32 uid = 0;
            MIDIObjectGetIntegerProperty (src, kMIDIPropertyUniqueID, &uid);

            if (byUniqueId ? juce::String (uid) == sourceName : n == sourceName)
            {
                ++matchingSources;

                // Stay on the current one if it still exists
                if (match == 0 || src == connected)
                    match = src;
            }
        }

        if (match == 0)
            return false;

        if (match == connected)
            return true;

        if (connected != 0)
            MIDIPortDisconnectSource (port, connected);

        connected = match;
        return MIDIPortConnectSource (port, match, nullptr) == noErr;
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
    if (! processor.isPortConnected() || ! receiver.connect (processor.getOpenPortIdentifier(), true))
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
    std::vector<double> jumpTimes; // wall time of each start / locate

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
            {
                playHead.position = cues[nextCue].jumpTo;
                jumpTimes.push_back (due);
            }
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
    int staleCycles = 0, unstamped = 0, qfCount = 0;
    double sumAheadMs = 0.0;

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

        ++qfCount;
        sumAheadMs += e.wallMs - e.arrivalMs;
        if (! e.stamped)
            ++unstamped;

        // Up to the lookahead's worth may still be stamped just after a stop
        bool playingEarlier = false;
        hostPositionAt (blocks, e.wallMs - 30.0, playingEarlier);
        if (! playing && ! playingEarlier)
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

            // A cycle already in flight when the host jumped is stale by
            // design (lookahead); the full frame after it corrects the receiver.
            bool straddlesJump = false;
            for (auto j : jumpTimes)
                straddlesJump = straddlesJump || (j >= pieceTimes[0] - 1.0 && j <= pieceTimes[7]);

            if (straddlesJump)
            {
                ++staleCycles;
                continue;
            }

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
                 && maxPieceErrMs < 5.0 && unstamped == 0 && staleCycles <= (int) jumpTimes.size();

    std::printf ("  %-9s cycles %4d  full frames %d  QF timing error vs host: mean %+.3f ms, sd %.3f ms, max %.3f ms"
                 "  | stale %d, out-of-order %d, >1 frame off %d, after stop %d, sent ahead %.1f ms, audio %s  -> %s\n",
                 rateIndex == 2 ? "29.97df" : juce::String (mtc::nominalFps (rate)).toRawUTF8(),
                 cycles, fullFrames, mean, sd, maxPieceErrMs, staleCycles, outOfOrder, lateCycles, sentWhileStopped,
                 qfCount > 0 ? sumAheadMs / qfCount : 0.0,
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
// Live MTC reader: connect to a source, decode, print one status line per second
// plus a line for every full frame. Ctrl-C to quit.

static std::atomic<bool> quitRequested { false };

static int monitor (const juce::String& sourceName, double intervalSeconds)
{
    Receiver receiver;

    std::signal (SIGINT, [] (int) { quitRequested = true; });

    if (! receiver.connect (sourceName))
    {
        std::printf ("Waiting for MIDI source \"%s\" (load MTC4Live with its virtual port)...\n", sourceName.toRawUTF8());
        std::fflush (stdout);

        // CoreMIDI only refreshes this process's device list while a run loop runs
        while (! quitRequested && ! receiver.connect (sourceName))
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.25, false);

        if (quitRequested)
            return 0;
    }
    std::printf ("Listening to \"%s\" (Ctrl-C to quit)\n", sourceName.toRawUTF8());
    int warnedMatches = 1;
    std::fflush (stdout);

    static const char* rateNames[] { "24", "25", "29.97df", "30" };
    const auto startMs = juce::Time::getMillisecondCounterHiRes();
    auto clock = [&] (double ms)
    {
        return juce::String::formatted ("%8.3f s", (ms - startMs) * 0.001);
    };

    int nibbles[8] = {};
    int expectedPiece = -1, rateCode = -1;
    bool haveTimecode = false;
    mtc::Timecode current;
    double lastQfStamp = 0.0;
    juce::String lastStoppedLine; // only print "stopped" when it changes

    while (! quitRequested)
    {
        const auto tickStart = juce::Time::getMillisecondCounterHiRes();

        for (auto elapsed = 0.0; elapsed < intervalSeconds && ! quitRequested;
             elapsed = (juce::Time::getMillisecondCounterHiRes() - tickStart) * 0.001)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, juce::jmin (0.05, intervalSeconds - elapsed), false);

        std::vector<Received> events;
        {
            const std::lock_guard<std::mutex> sl (receiver.lock);
            events.swap (receiver.events);
        }

        int quarterFrames = 0, sequenceErrors = 0, unstamped = 0;
        double sumDev = 0.0, sumSqDev = 0.0, maxDev = 0.0, maxLateArrival = 0.0;
        int intervals = 0;

        for (const auto& e : events)
        {
            if (e.bytes.size() == 10 && e.bytes[0] == 0xF0 && e.bytes[1] == 0x7F && e.bytes[3] == 0x01 && e.bytes[4] == 0x01)
            {
                rateCode = (e.bytes[5] >> 5) & 3;
                current = { e.bytes[5] & 0x1f, e.bytes[6], e.bytes[7], e.bytes[8] };
                haveTimecode = true;
                expectedPiece = 0;
                lastQfStamp = 0.0;
                std::printf ("%s  FULL FRAME  %s  (%s fps)\n", clock (e.wallMs).toRawUTF8(),
                             mtc::toString (current, mtc::frameRateFromIndex (rateCode)).c_str(), rateNames[rateCode]);
                continue;
            }

            if (e.bytes.size() != 2 || e.bytes[0] != 0xF1)
                continue;

            ++quarterFrames;
            unstamped += e.stamped ? 0 : 1;
            maxLateArrival = juce::jmax (maxLateArrival, e.arrivalMs - e.wallMs);

            const int piece = e.bytes[1] >> 4;
            if (expectedPiece >= 0 && piece != expectedPiece)
                ++sequenceErrors;
            expectedPiece = (piece + 1) & 7;
            nibbles[piece] = e.bytes[1] & 0x0f;

            if (piece == 7)
            {
                rateCode = (nibbles[7] >> 1) & 3;
                const mtc::Timecode tc { nibbles[6] | ((nibbles[7] & 1) << 4), nibbles[4] | (nibbles[5] << 4),
                                         nibbles[2] | (nibbles[3] << 4), nibbles[0] | (nibbles[1] << 4) };
                const auto rate = mtc::frameRateFromIndex (rateCode);
                current = mtc::framesToTimecode (mtc::timecodeToFrames (tc, rate) + 2, rate);
                haveTimecode = true;
            }

            if (lastQfStamp > 0.0 && rateCode >= 0)
            {
                const auto ideal = 1000.0 / (4.0 * mtc::actualFps (mtc::frameRateFromIndex (rateCode)));
                const auto dev = (e.wallMs - lastQfStamp) - ideal;
                sumDev += dev;
                sumSqDev += dev * dev;
                maxDev = juce::jmax (maxDev, std::abs (dev));
                ++intervals;
            }

            lastQfStamp = e.wallMs;
        }

        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (receiver.matchingSources > 1 && receiver.matchingSources != warnedMatches)
            std::printf ("%s  WARNING: %d sources named \"%s\" (several MTC4Live instances?). Listening to one of them.\n",
                         clock (now).toRawUTF8(), receiver.matchingSources, sourceName.toRawUTF8());
        warnedMatches = juce::jmax (1, receiver.matchingSources);

        if (quarterFrames == 0)
        {
            // The port may have been recreated (plugin re-added, output switched)
            const auto before = receiver.connected;
            if (receiver.connect (sourceName) && receiver.connected != before)
            {
                std::printf ("%s  reconnected to \"%s\"\n", clock (now).toRawUTF8(), sourceName.toRawUTF8());
                expectedPiece = -1; // joining a new stream mid-cycle
            }

            const juce::String stoppedLine = haveTimecode && rateCode >= 0
                ? juce::String (mtc::toString (current, mtc::frameRateFromIndex (rateCode)))
                : juce::String ("--:--:--:--");

            if (stoppedLine != lastStoppedLine)
            {
                std::printf ("%s  stopped     %s\n", clock (now).toRawUTF8(), stoppedLine.toRawUTF8());
                lastStoppedLine = stoppedLine;
            }

            lastQfStamp = 0.0;
            expectedPiece = -1;
        }
        else
        {
            const auto mean = intervals > 0 ? sumDev / intervals : 0.0;
            const auto sd = intervals > 0 ? std::sqrt (juce::jmax (0.0, sumSqDev / intervals - mean * mean)) : 0.0;

            lastStoppedLine = {};
            const auto qfPerSecond = quarterFrames / juce::jmax (0.001, (now - tickStart) * 0.001);

            std::printf ("%s  RUNNING     %s  %s fps  %3.0f QF/s  interval jitter sd %.2f ms max %.2f ms"
                         "  late arrival max %.2f ms%s%s\n",
                         clock (now).toRawUTF8(),
                         mtc::toString (current, mtc::frameRateFromIndex (juce::jmax (0, rateCode))).c_str(),
                         rateCode >= 0 ? rateNames[rateCode] : "?", qfPerSecond, sd, maxDev, maxLateArrival,
                         sequenceErrors > 0 ? juce::String::formatted ("  SEQUENCE ERRORS %d", sequenceErrors).toRawUTF8() : "",
                         unstamped > 0 ? "  (unstamped)" : "");
        }

        std::fflush (stdout);
    }

    return 0;
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    if (argc >= 3 && juce::String (argv[1]) == "snapshot")
        return snapshot (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    if (argc >= 2 && juce::String (argv[1]) == "e2e")
        return e2e();

    if (argc >= 2 && juce::String (argv[1]) == "monitor")
    {
        juce::String source (MTC4LiveProcessor::virtualPortName);
        double interval = 0.25;

        for (int i = 2; i < argc; ++i)
        {
            const juce::String arg (argv[i]);

            if (arg == "--interval" && i + 1 < argc)
                interval = juce::jlimit (0.05, 10.0, juce::String (argv[++i]).getDoubleValue());
            else
                source = arg;
        }

        return monitor (source, interval);
    }

    std::printf ("usage: MTC4LiveTool snapshot <out.png> | e2e | monitor [source name] [--interval seconds]\n");
    return 2;
}
