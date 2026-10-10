#include "PluginEditor.h"

namespace
{
    // Port combo item ids
    constexpr int noneId = 1;
    constexpr int virtualId = 2;
    constexpr int missingDeviceId = 3;
    constexpr int firstDeviceId = 10;

    const juce::Colour background { 0xff1d2024 };
    const juce::Colour panel      { 0xff272b31 };
    const juce::Colour accent     { 0xff4fc3f7 };
    const juce::Colour playing    { 0xff66bb6a };
    const juce::Colour warning    { 0xffffa726 };
    const juce::Colour dim        { 0xff8a919a };

    void styleRowLabel (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, dim);
        label.setJustificationType (juce::Justification::centredLeft);
    }
}

MTC4LiveEditor::MTC4LiveEditor (MTC4LiveProcessor& p)
    : AudioProcessorEditor (&p), mtcProcessor (p)
{
    timecodeLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 46.0f, juce::Font::bold));
    timecodeLabel.setJustificationType (juce::Justification::centred);
    timecodeLabel.setColour (juce::Label::textColourId, accent);
    addAndMakeVisible (timecodeLabel);

    for (auto* l : { &statusLabel, &portStatusLabel })
    {
        l->setJustificationType (juce::Justification::centred);
        l->setFont (juce::FontOptions (14.0f, juce::Font::bold));
        addAndMakeVisible (*l);
    }

    styleRowLabel (portLabel, "Output");
    styleRowLabel (rateLabel, "Frame rate");
    styleRowLabel (offsetLabel, "Start offset");
    styleRowLabel (latencyLabel, "Latency (ms)");

    for (auto* l : { &portLabel, &rateLabel, &offsetLabel, &latencyLabel })
        addAndMakeVisible (*l);

    noteLabel.setText ("Positive latency sends MTC earlier. Use only one active instance per project.",
                       juce::dontSendNotification);
    noteLabel.setFont (juce::FontOptions (12.0f));
    noteLabel.setColour (juce::Label::textColourId, dim);
    noteLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (noteLabel);

    // Output port
    portBox.onChange = [this] { portSelectionChanged(); };
    addAndMakeVisible (portBox);

    refreshButton.onClick = [this] { refreshPortList(); };
    addAndMakeVisible (refreshButton);

    // Frame rate (item ids must be index + 1 for the attachment)
    rateBox.addItemList ({ "24", "25", "29.97 drop-frame", "30" }, 1);
    addAndMakeVisible (rateBox);
    rateAttachment = std::make_unique<ComboAttachment> (mtcProcessor.parameters, "frameRate", rateBox);

    // Start offset
    offsetEditor.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 16.0f, juce::Font::plain));
    offsetEditor.setJustification (juce::Justification::centred);
    offsetEditor.setInputRestrictions (11, "0123456789:;.");
    offsetEditor.setText (mtcProcessor.getStartOffsetString(), false);
    offsetEditor.onReturnKey = [this] { commitStartOffset(); unfocusAllComponents(); };
    offsetEditor.onFocusLost = [this] { commitStartOffset(); };
    offsetEditor.onEscapeKey = [this]
    {
        offsetEditor.setText (mtcProcessor.getStartOffsetString(), false);
        unfocusAllComponents();
    };
    addAndMakeVisible (offsetEditor);

    // Latency
    latencySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    addAndMakeVisible (latencySlider);
    latencyAttachment = std::make_unique<SliderAttachment> (mtcProcessor.parameters, "latencyMs", latencySlider);
    latencySlider.setDoubleClickReturnValue (true, 0.0);

    addAndMakeVisible (activeToggle);
    addAndMakeVisible (fullFrameToggle);
    activeAttachment = std::make_unique<ButtonAttachment> (mtcProcessor.parameters, "active", activeToggle);
    fullFrameAttachment = std::make_unique<ButtonAttachment> (mtcProcessor.parameters, "fullFrameWhenStopped", fullFrameToggle);

    refreshPortList();
    timerCallback();

    setSize (520, 420);
    startTimerHz (30);
}

MTC4LiveEditor::~MTC4LiveEditor()
{
    stopTimer();
}

void MTC4LiveEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto area = getLocalBounds().reduced (12);
    g.setColour (panel);
    g.fillRoundedRectangle (area.removeFromTop (150).toFloat(), 8.0f);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText ("MTC4Live", getLocalBounds().reduced (24, 18).removeFromTop (20), juce::Justification::topLeft);

    g.setColour (dim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("MIDI Time Code generator", getLocalBounds().reduced (24, 18).removeFromTop (20),
                juce::Justification::topRight);
}

void MTC4LiveEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto header = area.removeFromTop (150).reduced (12);
    header.removeFromTop (22);
    timecodeLabel.setBounds (header.removeFromTop (64));
    auto statusRow = header.removeFromTop (28);
    statusLabel.setBounds (statusRow.removeFromLeft (statusRow.getWidth() / 2));
    portStatusLabel.setBounds (statusRow);

    area.removeFromTop (12);

    const int rowHeight = 30, labelWidth = 100, gap = 8;

    auto row = [&] { auto r = area.removeFromTop (rowHeight); area.removeFromTop (gap); return r; };

    auto r = row();
    portLabel.setBounds (r.removeFromLeft (labelWidth));
    refreshButton.setBounds (r.removeFromRight (80));
    r.removeFromRight (gap);
    portBox.setBounds (r);

    r = row();
    rateLabel.setBounds (r.removeFromLeft (labelWidth));
    rateBox.setBounds (r.removeFromLeft (200));

    r = row();
    offsetLabel.setBounds (r.removeFromLeft (labelWidth));
    offsetEditor.setBounds (r.removeFromLeft (140));

    r = row();
    latencyLabel.setBounds (r.removeFromLeft (labelWidth));
    latencySlider.setBounds (r);

    r = row();
    r.removeFromLeft (labelWidth);
    activeToggle.setBounds (r.removeFromLeft (100));
    fullFrameToggle.setBounds (r);

    noteLabel.setBounds (area.removeFromBottom (20));
}

void MTC4LiveEditor::timerCallback()
{
    const auto rate = mtcProcessor.getFrameRate();
    timecodeLabel.setText (mtc::toString (mtcProcessor.getCurrentTimecode(), rate), juce::dontSendNotification);

    juce::String status;
    juce::Colour statusColour = dim;

    if (! mtcProcessor.isActive())
    {
        status = "INACTIVE";
    }
    else if (mtcProcessor.isGenerating())
    {
        status = "PLAYING - sending MTC";
        statusColour = playing;
    }
    else
    {
        status = mtcProcessor.isHostPlaying() ? "PLAYING" : "STOPPED";
    }

    statusLabel.setText (status, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, statusColour);

    const auto mode = mtcProcessor.getPortMode();
    const bool connected = mtcProcessor.isPortConnected();

    if (mode == MTC4LiveProcessor::PortMode::none)
    {
        portStatusLabel.setText ("No output selected", juce::dontSendNotification);
        portStatusLabel.setColour (juce::Label::textColourId, warning);
    }
    else
    {
        portStatusLabel.setText (connected ? "Port connected" : "Port not connected", juce::dontSendNotification);
        portStatusLabel.setColour (juce::Label::textColourId, connected ? playing : warning);
    }

    if (! offsetEditor.hasKeyboardFocus (true))
    {
        const auto text = mtcProcessor.getStartOffsetString();
        if (offsetEditor.getText() != text)
            offsetEditor.setText (text, false);
    }
}

void MTC4LiveEditor::refreshPortList()
{
    listedDevices = CoreMidiOutput::getDestinations();

    portBox.clear (juce::dontSendNotification);
    portBox.addItem ("None", noneId);
    portBox.addItem (juce::String ("Virtual port: ") + MTC4LiveProcessor::virtualPortName, virtualId);
    portBox.addSeparator();

    const auto mode = mtcProcessor.getPortMode();
    const auto selectedId = mtcProcessor.getSelectedPortIdentifier();
    const auto selectedName = mtcProcessor.getSelectedPortName();
    int selectedItem = mode == MTC4LiveProcessor::PortMode::none ? noneId : virtualId;
    bool found = false;

    for (int i = 0; i < listedDevices.size(); ++i)
    {
        const auto& device = listedDevices.getReference (i);

        portBox.addItem (device.name, firstDeviceId + i);

        if (mode == MTC4LiveProcessor::PortMode::device && ! found
            && (device.identifier == selectedId || device.name == selectedName))
        {
            selectedItem = firstDeviceId + i;
            found = device.identifier == selectedId;
        }
    }

    if (mode == MTC4LiveProcessor::PortMode::device && selectedItem < firstDeviceId)
    {
        portBox.addItem (selectedName + " (not available)", missingDeviceId);
        selectedItem = missingDeviceId;
    }

    portBox.setSelectedId (selectedItem, juce::dontSendNotification);
}

void MTC4LiveEditor::portSelectionChanged()
{
    const auto id = portBox.getSelectedId();

    if (id == noneId)
        mtcProcessor.selectPort (MTC4LiveProcessor::PortMode::none);
    else if (id == virtualId)
        mtcProcessor.selectPort (MTC4LiveProcessor::PortMode::virtualPort);
    else if (id >= firstDeviceId && id - firstDeviceId < listedDevices.size())
    {
        const auto& device = listedDevices.getReference (id - firstDeviceId);
        mtcProcessor.selectPort (MTC4LiveProcessor::PortMode::device, device.identifier, device.name);
    }
}

void MTC4LiveEditor::commitStartOffset()
{
    // Invalid input just reverts to the current value
    mtcProcessor.setStartOffset (offsetEditor.getText());
    offsetEditor.setText (mtcProcessor.getStartOffsetString(), false);
}
