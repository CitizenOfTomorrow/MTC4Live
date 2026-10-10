#pragma once

// Pure timecode maths. No JUCE dependencies so it can be unit tested standalone.

#include <cstdint>
#include <optional>
#include <string>

namespace mtc
{

// Values match the MTC rate code (bits 5-6 of the full-frame hours byte,
// bits 1-2 of quarter-frame piece 7's nibble).
enum class FrameRate : int
{
    fps24     = 0,
    fps25     = 1,
    fps2997df = 2,
    fps30     = 3
};

struct Timecode
{
    int hours = 0, minutes = 0, seconds = 0, frames = 0;

    bool operator== (const Timecode& o) const noexcept
    {
        return hours == o.hours && minutes == o.minutes && seconds == o.seconds && frames == o.frames;
    }
    bool operator!= (const Timecode& o) const noexcept { return ! (*this == o); }
};

FrameRate frameRateFromIndex (int index) noexcept;
int rateCode (FrameRate) noexcept;

/** Frame labels per second: 24, 25, 30, 30. */
int nominalFps (FrameRate) noexcept;

/** Real frames per second: 24, 25, 30000/1001, 30. */
double actualFps (FrameRate) noexcept;

/** Number of distinct frames in 24 hours of timecode. */
int64_t framesPerDay (FrameRate) noexcept;

/** Wraps any (possibly negative) frame count into [0, framesPerDay). */
int64_t wrapFrames (int64_t frameCount, FrameRate) noexcept;

/** Frame count since 00:00:00:00 -> timecode. Wraps at 24h. */
Timecode framesToTimecode (int64_t frameCount, FrameRate) noexcept;

/** Timecode -> frame count since 00:00:00:00. Drop-frame labels that do not
    exist (frames 00/01 of a non-tenth minute) are treated as frame 02. */
int64_t timecodeToFrames (const Timecode&, FrameRate) noexcept;

/** Index of the frame containing the given time (floor). May be negative. */
int64_t secondsToFrames (double seconds, FrameRate) noexcept;

/** Start time of the given frame index in seconds. */
double framesToSeconds (int64_t frameIndex, FrameRate) noexcept;

/** Host position + start offset -> timecode, wrapped at 24h. */
Timecode secondsToTimecode (double seconds, const Timecode& offset, FrameRate) noexcept;

/** True if this label exists for the given rate. */
bool isValid (const Timecode&, FrameRate) noexcept;

/** "HH:MM:SS:FF", or "HH:MM:SS;FF" for drop-frame. */
std::string toString (const Timecode&, FrameRate);

/** Parses "HH:MM:SS:FF" (':' ';' '.' accepted as separators). Range checks
    hours/minutes/seconds and frames < 30, but not rate-specific validity. */
std::optional<Timecode> parse (const std::string&);

/** Packs into one 32-bit word (for passing between threads via an atomic). */
uint32_t pack (const Timecode&) noexcept;
Timecode unpack (uint32_t) noexcept;

} // namespace mtc
