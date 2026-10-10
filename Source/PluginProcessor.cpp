#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    namespace ids
    {
        const juce::String frameRate        { "frameRate" };
        const juce::String latency          { "latencyMs" };
        const juce::String active           { "active" };
        const juce::String fullFrameStopped { "fullFrameWhenStopped" };

        const juce::Identifier portMode     { "portMode" };
        const juce::Identifier portId       { "portId" };
        const juce::Identifier portName     { "portName" };
        const juce::Identifier startOffset  { "startOffset" };
    }

    constexpr int defaultFrameRateIndex = 1; // 25 fps
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout MTC4LiveProcessor::createLayout()
{
    using namespace juce;

    return {
        std::make_unique<AudioParameterChoice> (ParameterID { ids::frameRate, 1 }, "Frame Rate",
                                                StringArray { "24", "25", "29.97 DF", "30" },
                                                defaultFrameRateIndex),
        std::make_unique<AudioParameterFloat> (ParameterID { ids::latency, 1 }, "Latency Offset",
                                               NormalisableRange<float> (-500.0f, 500.0f, 0.5f), 0.0f,
                                               AudioParameterFloatAttributes().withLabel ("ms")),
        std::make_unique<AudioParameterBool> (ParameterID { ids::active, 1 }, "Active", true),
        std::make_unique<AudioParameterBool> (ParameterID { ids::fullFrameStopped, 1 },
                                              "Full Frame When Stopped", true),
    };
}

MTC4LiveProcessor::MTC4LiveProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "MTC4Live", createLayout()),
      frameRateParam (parameters.getRawParameterValue (ids::frameRate)),
      activeParam (parameters.getRawParameterValue (ids::active)),
      sender (transport, { parameters.getRawParameterValue (ids::frameRate),
                           parameters.getRawParameterValue (ids::latency),
                           parameters.getRawParameterValue (ids::active),
                           parameters.getRawParameterValue (ids::fullFrameStopped) })
{
    sender.startSending();

    // Port is opened on the message thread
    triggerAsyncUpdate();
}

MTC4LiveProcessor::~MTC4LiveProcessor()
{
    cancelPendingUpdate();
    deviceListConnection.reset();
}

//==============================================================================
void MTC4LiveProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    audioState.wasPlaying = false;
}

bool MTC4LiveProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void MTC4LiveProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)  { process (buffer); }
void MTC4LiveProcessor::processBlock (juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) { process (buffer); }

template <typename Sample>
void MTC4LiveProcessor::process (juce::AudioBuffer<Sample>& buffer)
{
    // Audio passes through untouched (in-place buffer). Real-time safe from
    // here on: no locks, no allocation, no MIDI I/O.
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const auto numSamples = buffer.getNumSamples();
    const auto now = juce::Time::getMillisecondCounterHiRes();
    auto& st = audioState;

    const auto lastBlockSeconds = st.lastNumSamples / currentSampleRate;
    const auto expected = st.lastPosition + lastBlockSeconds;

    bool playing = false;
    double position = st.lastPosition;

    if (! isNonRealtime())
    {
        if (auto* playHead = getPlayHead())
        {
            if (const auto info = playHead->getPosition())
            {
                playing = info->getIsPlaying();

                if (const auto seconds = info->getTimeInSeconds())
                {
                    position = *seconds;
                }
                else if (const auto ppq = info->getPpqPosition())
                {
                    // No time in seconds: derive it from PPQ. While PPQ moves
                    // continuously, integrate block lengths so tempo changes
                    // do not look like jumps.
                    const auto bpm = juce::jmax (1.0, info->getBpm().orFallback (st.lastBpm));
                    const auto expectedPpq = st.lastPpq + lastBlockSeconds * st.lastBpm / 60.0;
                    const auto halfFrameBeats = 0.5 / 30.0 * bpm / 60.0;

                    if (st.wasPlaying && playing && std::abs (*ppq - expectedPpq) < halfFrameBeats)
                        position = expected;
                    else
                        position = *ppq * 60.0 / bpm;

                    st.lastPpq = *ppq;
                    st.lastBpm = bpm;
                }
                else if (playing)
                {
                    position = st.wasPlaying ? expected : 0.0;
                }
            }
        }
    }

    if (playing)
    {
        const auto fps = mtc::actualFps (mtc::frameRateFromIndex (static_cast<int> (frameRateParam->load())));

        // Start, locate or loop wrap -> sender sends full frame and restarts
        // the quarter-frame cycle.
        if (! st.wasPlaying || std::abs (position - expected) > 0.5 / fps)
            ++st.discontinuity;
    }

    st.wasPlaying = playing;
    st.lastPosition = position;
    st.lastNumSamples = numSamples;

    transport.write ({ playing, position, now, st.discontinuity, ++st.blockCounter });
}

//==============================================================================
juce::AudioProcessorEditor* MTC4LiveProcessor::createEditor()
{
    return new MTC4LiveEditor (*this);
}

void MTC4LiveProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();

    {
        const juce::ScopedLock sl (settingsLock);
        state.setProperty (ids::portMode, static_cast<int> (portMode), nullptr);
        state.setProperty (ids::portId, portIdentifier, nullptr);
        state.setProperty (ids::portName, portName, nullptr);
    }

    state.setProperty (ids::startOffset, juce::String (mtc::toString (sender.getStartOffset(), mtc::FrameRate::fps30)), nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MTC4LiveProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    parameters.replaceState (state);

    {
        const juce::ScopedLock sl (settingsLock);
        portMode = static_cast<PortMode> (juce::jlimit (0, 2, static_cast<int> (state.getProperty (ids::portMode, 1))));
        portIdentifier = state.getProperty (ids::portId).toString();
        portName = state.getProperty (ids::portName).toString();
    }

    setStartOffset (state.getProperty (ids::startOffset, "01:00:00:00").toString());

    // Hosts may call this off the message thread; open the port there.
    triggerAsyncUpdate();
}

//==============================================================================
void MTC4LiveProcessor::selectPort (PortMode mode, const juce::String& identifier, const juce::String& name)
{
    JUCE_ASSERT_MESSAGE_THREAD

    {
        const juce::ScopedLock sl (settingsLock);
        portMode = mode;
        portIdentifier = identifier;
        portName = name;
    }

    openSelectedPort();
}

MTC4LiveProcessor::PortMode MTC4LiveProcessor::getPortMode() const
{
    const juce::ScopedLock sl (settingsLock);
    return portMode;
}

juce::String MTC4LiveProcessor::getSelectedPortIdentifier() const
{
    const juce::ScopedLock sl (settingsLock);
    return portIdentifier;
}

juce::String MTC4LiveProcessor::getSelectedPortName() const
{
    const juce::ScopedLock sl (settingsLock);
    return portName;
}

bool MTC4LiveProcessor::setStartOffset (const juce::String& text)
{
    const auto tc = mtc::parse (text.toStdString());

    if (! tc)
        return false;

    sender.setStartOffset (*tc);
    return true;
}

juce::String MTC4LiveProcessor::getStartOffsetString() const
{
    return mtc::toString (sender.getStartOffset(), getFrameRate());
}

mtc::FrameRate MTC4LiveProcessor::getFrameRate() const
{
    return mtc::frameRateFromIndex (static_cast<int> (frameRateParam->load()));
}

//==============================================================================
void MTC4LiveProcessor::handleAsyncUpdate()
{
    if (! listeningForDevices)
    {
        deviceListConnection = juce::MidiDeviceListConnection::make ([this] { openSelectedPort(); });
        listeningForDevices = true;
    }

    openSelectedPort();
}

void MTC4LiveProcessor::openSelectedPort()
{
    JUCE_ASSERT_MESSAGE_THREAD

    PortMode mode;
    juce::String wantedId, wantedName;

    {
        const juce::ScopedLock sl (settingsLock);
        mode = portMode;
        wantedId = portIdentifier;
        wantedName = portName;
    }

    if (mode == PortMode::none)
    {
        openedMode = mode;
        sender.setOutput (nullptr);
        return;
    }

    if (mode == PortMode::virtualPort)
    {
        // Creating the port fires a device-list change; don't recreate it then.
        if (openedMode == PortMode::virtualPort && sender.hasOutput())
            return;

        openedMode = mode;
        sender.setOutput (CoreMidiOutput::createVirtualSource (virtualPortName));
        return;
    }

    // Hardware / IAC device: match by identifier, fall back to name (identifiers
    // can change between sessions or machines).
    juce::String foundId;

    for (const auto& device : CoreMidiOutput::getDestinations())
    {
        if (device.identifier == wantedId)
        {
            foundId = device.identifier;
            break;
        }

        if (foundId.isEmpty() && device.name == wantedName)
            foundId = device.identifier;
    }

    if (foundId.isEmpty())
    {
        // Unplugged (or not present yet): close, reopen when it reappears.
        openedMode = mode;
        sender.setOutput (nullptr);
        return;
    }

    if (openedMode == PortMode::device && sender.hasOutput() && sender.getOutputIdentifier() == foundId)
        return;

    {
        const juce::ScopedLock sl (settingsLock);
        portIdentifier = foundId;
    }

    openedMode = mode;
    sender.setOutput (CoreMidiOutput::openDestination (foundId));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MTC4LiveProcessor();
}
