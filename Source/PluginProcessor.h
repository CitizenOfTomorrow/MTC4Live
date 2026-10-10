#pragma once

#include "MtcSender.h"
#include "TransportExchange.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

/** Audio pass-through effect that turns the host transport into MIDI Time Code
    sent straight to a MIDI port (hardware, IAC or its own virtual port). */
class MTC4LiveProcessor : public juce::AudioProcessor,
                          private juce::AsyncUpdater
{
public:
    static constexpr const char* virtualPortName = "Live MTC";

    enum class PortMode { none, virtualPort, device };

    MTC4LiveProcessor();
    ~MTC4LiveProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==============================================================================
    // Message thread API for the editor
    void selectPort (PortMode mode, const juce::String& identifier = {}, const juce::String& name = {});
    PortMode getPortMode() const;
    juce::String getSelectedPortIdentifier() const;
    juce::String getSelectedPortName() const;
    bool isPortConnected() const { return sender.hasOutput(); }
    juce::String getOpenPortIdentifier() const { return sender.getOutputIdentifier(); } // CoreMIDI unique ID

    bool setStartOffset (const juce::String& text);
    juce::String getStartOffsetString() const;

    mtc::FrameRate getFrameRate() const;
    mtc::Timecode getCurrentTimecode() const { return sender.getCurrentTimecode(); }
    bool isGenerating() const               { return sender.isGenerating(); }
    bool isHostPlaying() const              { return transport.read().playing; }
    bool isActive() const                   { return activeParam->load() > 0.5f; }

    juce::AudioProcessorValueTreeState parameters;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    template <typename Sample>
    void process (juce::AudioBuffer<Sample>&);

    void handleAsyncUpdate() override;
    void openSelectedPort();

    std::atomic<float>* frameRateParam = nullptr;
    std::atomic<float>* activeParam = nullptr;

    TransportExchange transport;
    MtcSender sender;

    // Audio-thread-only transport tracking
    struct AudioThreadState
    {
        bool wasPlaying = false;
        double lastPosition = 0.0;
        double lastPpq = 0.0;
        double lastBpm = 120.0;
        int lastNumSamples = 0;
        uint32_t discontinuity = 0;
        uint32_t blockCounter = 0;
    } audioState;

    double currentSampleRate = 44100.0;

    // Port selection (non-automatable state). Guarded because hosts may save
    // state off the message thread.
    juce::CriticalSection settingsLock;
    PortMode portMode = PortMode::virtualPort;
    juce::String portIdentifier, portName;

    PortMode openedMode = PortMode::none; // message thread only
    juce::MidiDeviceListConnection deviceListConnection;
    bool listeningForDevices = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MTC4LiveProcessor)
};
