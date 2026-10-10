// Standalone unit tests for the timecode maths and MTC encoding.
// Build target: mtc_tests (run via ctest).

#include "../Source/MtcMessages.h"
#include "../Source/TimecodeMath.h"

#include <cstdio>
#include <string>

using namespace mtc;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (! (cond)) {                                                          \
            ++failures;                                                          \
            std::printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (false)

#define CHECK_TC(actual, expectedString, rate)                                   \
    do {                                                                         \
        ++checks;                                                                \
        const auto s = toString ((actual), (rate));                              \
        if (s != (expectedString)) {                                             \
            ++failures;                                                          \
            std::printf ("FAIL %s:%d: got %s, expected %s\n",                    \
                         __FILE__, __LINE__, s.c_str(), expectedString);         \
        }                                                                        \
    } while (false)

static const FrameRate allRates[] = { FrameRate::fps24, FrameRate::fps25, FrameRate::fps2997df, FrameRate::fps30 };

static void testBasicRates()
{
    CHECK_TC (framesToTimecode (0, FrameRate::fps24), "00:00:00:00", FrameRate::fps24);
    CHECK_TC (framesToTimecode (23, FrameRate::fps24), "00:00:00:23", FrameRate::fps24);
    CHECK_TC (framesToTimecode (24, FrameRate::fps24), "00:00:01:00", FrameRate::fps24);
    CHECK_TC (framesToTimecode (24 * 3600, FrameRate::fps24), "01:00:00:00", FrameRate::fps24);

    CHECK_TC (framesToTimecode (24, FrameRate::fps25), "00:00:00:24", FrameRate::fps25);
    CHECK_TC (framesToTimecode (25 * 61 + 3, FrameRate::fps25), "00:01:01:03", FrameRate::fps25);

    CHECK_TC (framesToTimecode (1799, FrameRate::fps30), "00:00:59:29", FrameRate::fps30);
    CHECK_TC (framesToTimecode (1800, FrameRate::fps30), "00:01:00:00", FrameRate::fps30);
}

static void testDropFrame()
{
    const auto df = FrameRate::fps2997df;

    // The spec example: the frame after 00:00:59;29 is 00:01:00;02
    CHECK_TC (framesToTimecode (1799, df), "00:00:59;29", df);
    CHECK_TC (framesToTimecode (1800, df), "00:01:00;02", df);
    CHECK_TC (framesToTimecode (1801, df), "00:01:00;03", df);

    // Tenth minute keeps frames 00 and 01
    CHECK_TC (framesToTimecode (17981, df), "00:09:59;29", df);
    CHECK_TC (framesToTimecode (17982, df), "00:10:00;00", df);
    CHECK_TC (framesToTimecode (17983, df), "00:10:00;01", df);
    CHECK_TC (framesToTimecode (17984, df), "00:10:00;02", df);
    CHECK_TC (framesToTimecode (17982 + 1798 + 1, df), "00:10:59;29", df);
    CHECK_TC (framesToTimecode (17982 + 1798 + 2, df), "00:11:00;02", df);

    // One hour of 29.97 DF is 107892 frames
    CHECK_TC (framesToTimecode (107892, df), "01:00:00;00", df);
    CHECK (timecodeToFrames ({ 1, 0, 0, 0 }, df) == 107892);

    // Wall-clock accuracy: 10 minutes of real time is exactly 00:10:00;00
    CHECK_TC (secondsToTimecode (600.0, {}, df), "00:10:00;00", df);
    CHECK_TC (secondsToTimecode (3600.0, {}, df), "01:00:00;00", df);

    // Non-existent labels get bumped to frame 02
    CHECK (timecodeToFrames ({ 0, 1, 0, 0 }, df) == 1800);
    CHECK (timecodeToFrames ({ 0, 1, 0, 1 }, df) == 1800);
    CHECK (! isValid ({ 0, 1, 0, 0 }, df));
    CHECK (isValid ({ 0, 10, 0, 0 }, df));
    CHECK (isValid ({ 0, 1, 0, 2 }, df));
}

static void testRoundTripAllFrames()
{
    for (auto rate : allRates)
    {
        const auto day = framesPerDay (rate);
        bool ok = true;
        Timecode prev = framesToTimecode (day - 1, rate);

        for (int64_t n = 0; n < day && ok; ++n)
        {
            const auto tc = framesToTimecode (n, rate);

            if (! isValid (tc, rate) || timecodeToFrames (tc, rate) != n)
            {
                std::printf ("  round trip broke at frame %lld (%s)\n", (long long) n, toString (tc, rate).c_str());
                ok = false;
            }

            // Labels must be strictly increasing (except at the day wrap)
            if (n > 0 && timecodeToFrames (prev, rate) + 1 != n)
                ok = false;

            prev = tc;
        }

        CHECK (ok);
    }

    CHECK (framesPerDay (FrameRate::fps24) == 2073600);
    CHECK (framesPerDay (FrameRate::fps25) == 2160000);
    CHECK (framesPerDay (FrameRate::fps2997df) == 2589408);
    CHECK (framesPerDay (FrameRate::fps30) == 2592000);
}

static void testWrapAndOffset()
{
    for (auto rate : allRates)
    {
        const auto day = framesPerDay (rate);
        CHECK_TC (framesToTimecode (day, rate), "00:00:00:00", FrameRate::fps24);
        CHECK (framesToTimecode (-1, rate) == framesToTimecode (day - 1, rate));
        CHECK (framesToTimecode (day - 1, rate).hours == 23);
        CHECK (framesToTimecode (day - 1, rate).frames == nominalFps (rate) - 1);
    }

    const Timecode oneHour { 1, 0, 0, 0 };
    CHECK_TC (secondsToTimecode (0.0, oneHour, FrameRate::fps25), "01:00:00:00", FrameRate::fps25);
    CHECK_TC (secondsToTimecode (1.5, oneHour, FrameRate::fps25), "01:00:01:12", FrameRate::fps25);
    CHECK_TC (secondsToTimecode (-0.04, oneHour, FrameRate::fps25), "00:59:59:24", FrameRate::fps25);
    CHECK_TC (secondsToTimecode (-0.01, {}, FrameRate::fps25), "23:59:59:24", FrameRate::fps25);
    CHECK_TC (secondsToTimecode (3600.0, { 23, 30, 0, 0 }, FrameRate::fps30), "00:30:00:00", FrameRate::fps30);

    // DF offset: start at 01:00:00;00 and play 60 s -> 01:01:00;02
    CHECK_TC (secondsToTimecode (60.0, oneHour, FrameRate::fps2997df), "01:00:59;28", FrameRate::fps2997df);
    CHECK_TC (framesToTimecode (timecodeToFrames (oneHour, FrameRate::fps2997df) + 1800, FrameRate::fps2997df),
              "01:01:00;02", FrameRate::fps2997df);
}

static void testSecondsToFrames()
{
    // Exact frame boundaries must not fall into the previous frame
    for (int n = 0; n < 100000; ++n)
    {
        for (auto rate : allRates)
        {
            if (secondsToFrames (framesToSeconds (n, rate), rate) != n)
            {
                CHECK (false);
                return;
            }
        }
    }

    CHECK (secondsToFrames (0.04, FrameRate::fps25) == 1);
    CHECK (secondsToFrames (0.0399, FrameRate::fps25) == 0);
    CHECK (secondsToFrames (-0.001, FrameRate::fps25) == -1);
}

static void testParse()
{
    CHECK (parse ("01:00:00:00") == (Timecode { 1, 0, 0, 0 }));
    CHECK (parse ("23:59:59;29") == (Timecode { 23, 59, 59, 29 }));
    CHECK (parse ("1:2:3:4") == (Timecode { 1, 2, 3, 4 }));
    CHECK (! parse ("24:00:00:00"));
    CHECK (! parse ("01:00:00"));
    CHECK (! parse ("01:00:00:00:00"));
    CHECK (! parse ("ab:00:00:00"));
    CHECK (! parse ("001:00:00:00"));
    CHECK (! parse (""));

    const Timecode tc { 12, 34, 56, 7 };
    CHECK (unpack (pack (tc)) == tc);
}

static void testMessages()
{
    const Timecode tc { 1, 23, 45, 17 };

    // 25 fps -> rate code 1
    CHECK (quarterFrameData (0, tc, FrameRate::fps25) == 0x01); // 17 = 0x11 -> low 1
    CHECK (quarterFrameData (1, tc, FrameRate::fps25) == 0x11); // high 1
    CHECK (quarterFrameData (2, tc, FrameRate::fps25) == 0x2D); // 45 = 0x2D -> low D
    CHECK (quarterFrameData (3, tc, FrameRate::fps25) == 0x32); // high 2
    CHECK (quarterFrameData (4, tc, FrameRate::fps25) == 0x47); // 23 = 0x17 -> low 7
    CHECK (quarterFrameData (5, tc, FrameRate::fps25) == 0x51); // high 1
    CHECK (quarterFrameData (6, tc, FrameRate::fps25) == 0x61); // hours low 1
    CHECK (quarterFrameData (7, tc, FrameRate::fps25) == 0x72); // hours high 0 | rate 1 << 1

    CHECK (quarterFrameData (7, { 23, 0, 0, 0 }, FrameRate::fps24) == 0x71);     // 23 = 0x17 -> high 1, rate 0
    CHECK (quarterFrameData (7, { 16, 0, 0, 0 }, FrameRate::fps2997df) == 0x75); // high 1 | 2 << 1
    CHECK (quarterFrameData (7, { 0, 0, 0, 0 }, FrameRate::fps30) == 0x76);      // 3 << 1

    const auto ff = fullFrameSysEx (tc, FrameRate::fps30);
    const uint8_t expected[10] = { 0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x61, 23, 45, 17, 0xF7 }; // 0x60 | 1
    bool same = true;
    for (int i = 0; i < 10; ++i)
        same = same && ff[(size_t) i] == expected[i];
    CHECK (same);

    CHECK (fullFrameSysEx ({ 10, 0, 0, 0 }, FrameRate::fps2997df)[5] == 0x4A);
}

// Decodes the quarter-frame stream like a receiver and checks it agrees with
// the frame actually playing when each cycle completes.
static void testGeneratorAgainstReceiver()
{
    for (auto rate : allRates)
    {
        const Timecode offset { 1, 0, 0, 0 };
        const auto offsetFrames = timecodeToFrames (offset, rate);

        // Start mid-way, on an odd frame, just before a DF minute boundary
        const int64_t startFrame = 1791;
        QuarterFrameGenerator gen;
        gen.restart (startFrame);

        int nibbles[8] = {};
        bool ok = true;

        for (int i = 0; i < 8 * 2000 && ok; ++i)
        {
            const double due = gen.nextDueFrame();
            const int piece = gen.nextPiece();
            const auto data = gen.next (offsetFrames, rate);

            ok = ok && (data >> 4) == piece;
            ok = ok && due == startFrame + i * 0.25;
            nibbles[piece] = data & 0x0f;

            if (piece == 7)
            {
                Timecode rx;
                rx.frames  = nibbles[0] | (nibbles[1] << 4);
                rx.seconds = nibbles[2] | (nibbles[3] << 4);
                rx.minutes = nibbles[4] | (nibbles[5] << 4);
                rx.hours   = nibbles[6] | ((nibbles[7] & 1) << 4);
                const int rxRate = (nibbles[7] >> 1) & 3;

                // Receiver adds 2 frames: that must be the frame starting
                // right now (when the next piece 0 goes out).
                const auto nowFrame = static_cast<int64_t> (gen.nextDueFrame());
                const auto expected = framesToTimecode (nowFrame + offsetFrames, rate);
                const auto received = framesToTimecode (timecodeToFrames (rx, rate) + 2, rate);

                ok = ok && rxRate == rateCode (rate) && received == expected;
                if (! ok)
                    std::printf ("  receiver mismatch: got %s expected %s\n",
                                 toString (received, rate).c_str(), toString (expected, rate).c_str());
            }
        }

        CHECK (ok);
    }
}

// Long-run DF check: 10+ minutes of continuous generation stays in lock with
// the wall clock (the "reference generator" property).
static void testDropFrameLongRun()
{
    const auto df = FrameRate::fps2997df;
    QuarterFrameGenerator gen;
    gen.restart (0);

    // 11 minutes of real time
    const double endSeconds = 11.0 * 60.0;
    int64_t lastCycleFrame = 0;

    while (framesToSeconds (static_cast<int64_t> (gen.nextDueFrame()), df) < endSeconds)
    {
        if (gen.nextPiece() == 0)
            lastCycleFrame = static_cast<int64_t> (gen.nextDueFrame());
        gen.next (0, df);
    }

    const auto t = framesToSeconds (lastCycleFrame, df);
    const auto tc = framesToTimecode (lastCycleFrame, df);
    const double tcSeconds = tc.hours * 3600.0 + tc.minutes * 60.0 + tc.seconds + tc.frames / 30.0;

    // DF labels stay within ~2 frames of real time (the residual 0.0006%/10 min drift)
    CHECK (t - tcSeconds < 0.1 && tcSeconds - t < 0.1);
}

int main()
{
    testBasicRates();
    testDropFrame();
    testRoundTripAllFrames();
    testWrapAndOffset();
    testSecondsToFrames();
    testParse();
    testMessages();
    testGeneratorAgainstReceiver();
    testDropFrameLongRun();

    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
