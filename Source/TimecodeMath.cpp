#include "TimecodeMath.h"

#include <cmath>
#include <cstdio>

namespace mtc
{

namespace
{
    // Drop-frame constants (29.97 fps, 30 labels per second)
    constexpr int64_t dfFramesPerMinute      = 30 * 60 - 2;          // 1798
    constexpr int64_t dfFramesPer10Minutes   = 30 * 600 - 9 * 2;     // 17982
    constexpr int64_t nominal30FramesPerHour = 30 * 3600;            // 108000

    // Small bias so that e.g. 0.04 s * 25 fps lands on frame 1, not 0.99999...
    constexpr double frameEpsilon = 1.0e-7;
}

FrameRate frameRateFromIndex (int index) noexcept
{
    switch (index)
    {
        case 0:  return FrameRate::fps24;
        case 1:  return FrameRate::fps25;
        case 2:  return FrameRate::fps2997df;
        default: return FrameRate::fps30;
    }
}

int rateCode (FrameRate r) noexcept { return static_cast<int> (r); }

int nominalFps (FrameRate r) noexcept
{
    switch (r)
    {
        case FrameRate::fps24: return 24;
        case FrameRate::fps25: return 25;
        default:               return 30;
    }
}

double actualFps (FrameRate r) noexcept
{
    if (r == FrameRate::fps2997df)
        return 30000.0 / 1001.0;

    return static_cast<double> (nominalFps (r));
}

int64_t framesPerDay (FrameRate r) noexcept
{
    if (r == FrameRate::fps2997df)
        return dfFramesPer10Minutes * 6 * 24; // 2589408

    return static_cast<int64_t> (nominalFps (r)) * 86400;
}

int64_t wrapFrames (int64_t frameCount, FrameRate r) noexcept
{
    const auto day = framesPerDay (r);
    auto wrapped = frameCount % day;
    return wrapped < 0 ? wrapped + day : wrapped;
}

Timecode framesToTimecode (int64_t frameCount, FrameRate r) noexcept
{
    auto n = wrapFrames (frameCount, r);
    const int64_t fps = nominalFps (r);

    if (r == FrameRate::fps2997df)
    {
        // Convert the real frame count to a 30 fps label count by adding back
        // the dropped labels: 2 per minute, except every tenth minute.
        const auto tens = n / dfFramesPer10Minutes;
        const auto rem  = n % dfFramesPer10Minutes;
        auto dropped = 18 * tens;

        if (rem >= 2)
            dropped += 2 * ((rem - 2) / dfFramesPerMinute);

        n += dropped;
    }

    Timecode tc;
    tc.frames  = static_cast<int> (n % fps);
    tc.seconds = static_cast<int> ((n / fps) % 60);
    tc.minutes = static_cast<int> ((n / (fps * 60)) % 60);
    tc.hours   = static_cast<int> (n / (fps * 3600));
    return tc;
}

int64_t timecodeToFrames (const Timecode& tc, FrameRate r) noexcept
{
    const int64_t fps = nominalFps (r);

    if (r == FrameRate::fps2997df)
    {
        const int64_t totalMinutes = 60 * tc.hours + tc.minutes;
        int64_t frames = tc.frames;

        if (tc.seconds == 0 && frames < 2 && (tc.minutes % 10) != 0)
            frames = 2;

        return nominal30FramesPerHour * tc.hours
             + 1800 * tc.minutes
             + 30 * tc.seconds
             + frames
             - 2 * (totalMinutes - totalMinutes / 10);
    }

    return ((tc.hours * 60 + tc.minutes) * 60 + tc.seconds) * fps + tc.frames;
}

int64_t secondsToFrames (double seconds, FrameRate r) noexcept
{
    return static_cast<int64_t> (std::floor (seconds * actualFps (r) + frameEpsilon));
}

double framesToSeconds (int64_t frameIndex, FrameRate r) noexcept
{
    return static_cast<double> (frameIndex) / actualFps (r);
}

Timecode secondsToTimecode (double seconds, const Timecode& offset, FrameRate r) noexcept
{
    return framesToTimecode (secondsToFrames (seconds, r) + timecodeToFrames (offset, r), r);
}

bool isValid (const Timecode& tc, FrameRate r) noexcept
{
    if (tc.hours < 0 || tc.hours > 23 || tc.minutes < 0 || tc.minutes > 59
        || tc.seconds < 0 || tc.seconds > 59 || tc.frames < 0 || tc.frames >= nominalFps (r))
        return false;

    if (r == FrameRate::fps2997df && tc.seconds == 0 && tc.frames < 2 && (tc.minutes % 10) != 0)
        return false;

    return true;
}

std::string toString (const Timecode& tc, FrameRate r)
{
    char buf[16];
    std::snprintf (buf, sizeof (buf), "%02d:%02d:%02d%c%02d",
                   tc.hours, tc.minutes, tc.seconds,
                   r == FrameRate::fps2997df ? ';' : ':', tc.frames);
    return buf;
}

std::optional<Timecode> parse (const std::string& text)
{
    int values[4] = {};
    int field = 0, digits = 0;

    for (auto c : text)
    {
        if (c >= '0' && c <= '9')
        {
            if (field > 3 || ++digits > 2)
                return std::nullopt;

            values[field] = values[field] * 10 + (c - '0');
        }
        else if (c == ':' || c == ';' || c == '.')
        {
            if (digits == 0)
                return std::nullopt;

            ++field;
            digits = 0;
        }
        else if (c != ' ')
        {
            return std::nullopt;
        }
    }

    if (field != 3 || digits == 0)
        return std::nullopt;

    Timecode tc { values[0], values[1], values[2], values[3] };

    if (tc.hours > 23 || tc.minutes > 59 || tc.seconds > 59 || tc.frames > 29)
        return std::nullopt;

    return tc;
}

uint32_t pack (const Timecode& tc) noexcept
{
    return (static_cast<uint32_t> (tc.hours & 0xff) << 24)
         | (static_cast<uint32_t> (tc.minutes & 0xff) << 16)
         | (static_cast<uint32_t> (tc.seconds & 0xff) << 8)
         |  static_cast<uint32_t> (tc.frames & 0xff);
}

Timecode unpack (uint32_t v) noexcept
{
    return { static_cast<int> ((v >> 24) & 0xff), static_cast<int> ((v >> 16) & 0xff),
             static_cast<int> ((v >> 8) & 0xff),  static_cast<int> (v & 0xff) };
}

} // namespace mtc
