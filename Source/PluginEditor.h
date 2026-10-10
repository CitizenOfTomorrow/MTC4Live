#pragma once

#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

class MTC4LiveEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit MTC4LiveEditor (MTC4LiveProcessor&);
    ~MTC4LiveEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshPortList();
    void portSelectionChanged();
    void commitStartOffset();

    MTC4LiveProcessor& mtcProcessor;

    juce::Label timecodeLabel, statusLabel, portStatusLabel, noteLabel;
    juce::Label portLabel, rateLabel, offsetLabel, latencyLabel;

    juce::ComboBox portBox;
    juce::TextButton refreshButton { "Refresh" };
    juce::ComboBox rateBox;
    juce::TextEditor offsetEditor;
    juce::Slider latencySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::ToggleButton activeToggle { "Active" };
    juce::ToggleButton fullFrameToggle { "Full frame when stopped" };

    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<ComboAttachment> rateAttachment;
    std::unique_ptr<SliderAttachment> latencyAttachment;
    std::unique_ptr<ButtonAttachment> activeAttachment, fullFrameAttachment;

    juce::Array<CoreMidiOutput::Destination> listedDevices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MTC4LiveEditor)
};
