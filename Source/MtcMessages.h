#pragma once

// MTC message encoding. No JUCE dependencies.

#include "TimecodeMath.h"

#include <array>
#include <cstdint>

namespace mtc
{

constexpr uint8_t quarterFrameStatus = 0xF1;

/** Data byte (0nnndddd) of quarter-frame piece 0-7 for the given timecode. */
inline uint8_t quarterFrameData (int piece, const Timecode& tc, FrameRate rate) noexcept
{
    int nibble = 0;

    switch (piece & 7)
    {
        case 0: nibble = tc.frames & 0x0f;          break;
        case 1: nibble = (tc.frames >> 4) & 0x01;   break;
        case 2: nibble = tc.seconds & 0x0f;         break;
        case 3: nibble = (tc.seconds >> 4) & 0x03;  break;
        case 4: nibble = tc.minutes & 0x0f;         break;
        case 5: nibble = (tc.minutes >> 4) & 0x03;  break;
        case 6: nibble = tc.hours & 0x0f;           break;
        case 7: nibble = ((tc.hours >> 4) & 0x01) | (rateCode (rate) << 1); break;
    }

    return static_cast<uint8_t> (((piece & 7) << 4) | nibble);
}

/** F0 7F 7F 01 01 hr mn sc fr F7 */
inline std::array<uint8_t, 10> fullFrameSysEx (const Timecode& tc, FrameRate rate) noexcept
{
    return { 0xF0, 0x7F, 0x7F, 0x01, 0x01,
             static_cast<uint8_t> ((rateCode (rate) << 5) | (tc.hours & 0x1f)),
             static_cast<uint8_t> (tc.minutes & 0x3f),
             static_cast<uint8_t> (tc.seconds & 0x3f),
             static_cast<uint8_t> (tc.frames & 0x1f),
             0xF7 };
}

/** Sequences quarter-frame messages. A cycle of 8 pieces spans 2 frames and
    starts on a frame boundary. Every piece of a cycle encodes the timecode of
    the frame at which piece 0 was sent; the receiver adds the 2 frames itself.
    Frame indices are relative to the host timeline (frame 0 = position 0 s). */
class QuarterFrameGenerator
{
public:
    void restart (int64_t firstFrame) noexcept
    {
        cycleFrame = firstFrame;
        piece = 0;
    }

    /** Host frame position (fractional) at which the next message is due. */
    double nextDueFrame() const noexcept { return static_cast<double> (cycleFrame) + piece * 0.25; }

    int nextPiece() const noexcept { return piece; }

    /** Returns the next data byte and advances. */
    uint8_t next (int64_t offsetFrames, FrameRate rate) noexcept
    {
        const auto tc = framesToTimecode (cycleFrame + offsetFrames, rate);
        const auto data = quarterFrameData (piece, tc, rate);

        if (++piece == 8)
        {
            piece = 0;
            cycleFrame += 2;
        }

        return data;
    }

private:
    int64_t cycleFrame = 0;
    int piece = 0;
};

} // namespace mtc
