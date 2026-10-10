#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>

/** Minimal CoreMIDI output using the legacy MIDIPacketList API, so SysEx goes
    out as raw bytes. (JUCE 8's MidiOutput converts to UMP, and its SysEx does
    not reliably reach some hardware drivers.)

    Create/destroy on the message thread. send() is thread-safe. */
class CoreMidiOutput
{
public:
    struct Destination
    {
        juce::String name;
        juce::String identifier; // CoreMIDI unique ID as a string
    };

    static juce::Array<Destination> getDestinations();

    /** Opens an existing destination (hardware, IAC, another app). */
    static std::unique_ptr<CoreMidiOutput> openDestination (const juce::String& identifier);

    /** Creates a virtual source other apps can receive from. */
    static std::unique_ptr<CoreMidiOutput> createVirtualSource (const juce::String& name);

    ~CoreMidiOutput();

    /** One complete MIDI message (or SysEx) per call. atMs is the time it
        should go out, on the Time::getMillisecondCounterHiRes() clock; it may
        be in the future (CoreMIDI schedules it) and 0 means now. */
    void send (const uint8_t* data, int size, double atMs = 0.0) const noexcept;

    /** Drops messages scheduled for the future but not delivered yet. Returns
        false if this output can't do that (virtual sources deliver straight
        away, timestamps included). */
    bool flushPending() const noexcept;

    const juce::String& getIdentifier() const noexcept { return identifier; }
    const juce::String& getName() const noexcept       { return name; }

private:
    struct Native;

    CoreMidiOutput (std::unique_ptr<Native>, juce::String identifier, juce::String name);

    std::unique_ptr<Native> native;
    juce::String identifier, name;

    JUCE_DECLARE_NON_COPYABLE (CoreMidiOutput)
};
