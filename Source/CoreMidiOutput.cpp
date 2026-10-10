#include "CoreMidiOutput.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

namespace
{
    juce::String cfToString (CFStringRef s)
    {
        if (s == nullptr)
            return {};

        const auto length = CFStringGetMaximumSizeForEncoding (CFStringGetLength (s), kCFStringEncodingUTF8) + 1;
        juce::HeapBlock<char> buffer (static_cast<size_t> (length), true);

        if (! CFStringGetCString (s, buffer, length, kCFStringEncodingUTF8))
            return {};

        return juce::String::fromUTF8 (buffer);
    }

    juce::String endpointName (MIDIEndpointRef endpoint)
    {
        CFStringRef s = nullptr;

        if (MIDIObjectGetStringProperty (endpoint, kMIDIPropertyDisplayName, &s) != noErr || s == nullptr)
            if (MIDIObjectGetStringProperty (endpoint, kMIDIPropertyName, &s) != noErr)
                return {};

        auto result = cfToString (s);
        CFRelease (s);
        return result;
    }

    SInt32 endpointUniqueId (MIDIEndpointRef endpoint)
    {
        SInt32 uid = 0;
        MIDIObjectGetIntegerProperty (endpoint, kMIDIPropertyUniqueID, &uid);
        return uid;
    }

    /** Time::getMillisecondCounterHiRes() value -> CoreMIDI host time. */
    MIDITimeStamp hostTimeForMs (double ms) noexcept
    {
        static const double ticksPerMs = []
        {
            mach_timebase_info_data_t tb;
            mach_timebase_info (&tb);
            return 1.0e6 * (double) tb.denom / (double) tb.numer;
        }();

        const auto nowTicks = mach_absolute_time();
        const auto deltaMs = ms - juce::Time::getMillisecondCounterHiRes();

        if (deltaMs <= 0.0)
            return nowTicks;

        return nowTicks + static_cast<MIDITimeStamp> (deltaMs * ticksPerMs);
    }

    struct CFStr
    {
        explicit CFStr (const juce::String& s) : ref (s.toCFString()) {}
        ~CFStr() { if (ref != nullptr) CFRelease (ref); }
        CFStringRef ref;
    };
}

struct CoreMidiOutput::Native
{
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;               // destination mode
    MIDIEndpointRef destination = 0;    // destination mode
    MIDIEndpointRef virtualSource = 0;  // virtual mode

    ~Native()
    {
        if (virtualSource != 0) MIDIEndpointDispose (virtualSource);
        if (port != 0)          MIDIPortDispose (port);
        if (client != 0)        MIDIClientDispose (client);
    }

    bool createClient()
    {
        CFStr clientName ("MTC4Live");
        return MIDIClientCreate (clientName.ref, nullptr, nullptr, &client) == noErr;
    }
};

CoreMidiOutput::CoreMidiOutput (std::unique_ptr<Native> n, juce::String id, juce::String nm)
    : native (std::move (n)), identifier (std::move (id)), name (std::move (nm))
{
}

CoreMidiOutput::~CoreMidiOutput() = default;

juce::Array<CoreMidiOutput::Destination> CoreMidiOutput::getDestinations()
{
    juce::Array<Destination> result;
    const auto count = MIDIGetNumberOfDestinations();

    for (ItemCount i = 0; i < count; ++i)
    {
        const auto endpoint = MIDIGetDestination (i);

        if (endpoint != 0)
            result.add ({ endpointName (endpoint), juce::String (endpointUniqueId (endpoint)) });
    }

    return result;
}

std::unique_ptr<CoreMidiOutput> CoreMidiOutput::openDestination (const juce::String& id)
{
    const auto count = MIDIGetNumberOfDestinations();

    for (ItemCount i = 0; i < count; ++i)
    {
        const auto endpoint = MIDIGetDestination (i);

        if (endpoint == 0 || juce::String (endpointUniqueId (endpoint)) != id)
            continue;

        auto n = std::make_unique<Native>();

        if (! n->createClient())
            return nullptr;

        CFStr portName ("MTC out");

        if (MIDIOutputPortCreate (n->client, portName.ref, &n->port) != noErr)
            return nullptr;

        n->destination = endpoint;
        return std::unique_ptr<CoreMidiOutput> (new CoreMidiOutput (std::move (n), id, endpointName (endpoint)));
    }

    return nullptr;
}

std::unique_ptr<CoreMidiOutput> CoreMidiOutput::createVirtualSource (const juce::String& sourceName)
{
    auto n = std::make_unique<Native>();

    if (! n->createClient())
        return nullptr;

    CFStr cfName (sourceName);

    if (MIDISourceCreate (n->client, cfName.ref, &n->virtualSource) != noErr)
        return nullptr;

    // Keep the port's unique ID stable across sessions so receivers that
    // remember it reconnect automatically. Ignore failure (ID collision).
    MIDIObjectSetIntegerProperty (n->virtualSource, kMIDIPropertyUniqueID,
                                  static_cast<SInt32> (juce::String ("MTC4Live:" + sourceName).hashCode()));

    const auto id = juce::String (endpointUniqueId (n->virtualSource));
    return std::unique_ptr<CoreMidiOutput> (new CoreMidiOutput (std::move (n), id, sourceName));
}

void CoreMidiOutput::send (const uint8_t* data, int size, double atMs) const noexcept
{
    if (data == nullptr || size <= 0 || size > 200)
        return;

    alignas (MIDIPacketList) Byte buffer[256];
    auto* packetList = reinterpret_cast<MIDIPacketList*> (buffer);
    auto* packet = MIDIPacketListInit (packetList);
    const MIDITimeStamp timeStamp = atMs > 0.0 ? hostTimeForMs (atMs) : 0;
    packet = MIDIPacketListAdd (packetList, sizeof (buffer), packet, timeStamp, static_cast<ByteCount> (size), data);

    if (packet == nullptr)
        return;

    if (native->virtualSource != 0)
        MIDIReceived (native->virtualSource, packetList);
    else
        MIDISend (native->port, native->destination, packetList);
}

bool CoreMidiOutput::flushPending() const noexcept
{
    if (native->virtualSource != 0)
        return false;

    return MIDIFlushOutput (native->destination) == noErr;
}
