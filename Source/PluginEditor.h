#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// Small toggle-group button used for CURVE / TARGET / SHAPE - flat dark background, lights up with
// the accent colour when selected. Kept intentionally simple for this first version.
class PDToggleButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    void paintButton(juce::Graphics& g, bool isMouseOver, bool) override {
        auto r=getLocalBounds().toFloat();
        bool on=getToggleState();
        g.setColour(on? juce::Colour(0xff1d2633) : juce::Colour(0xff10161e));
        g.fillRoundedRectangle(r,6.f);
        g.setColour(on? juce::Colour(0xff69a1d0) : (isMouseOver? juce::Colour(0xff3a4148):juce::Colour(0xff313942)));
        g.drawRoundedRectangle(r.reduced(0.75f),6.f,on?1.5f:1.f);
        g.setColour(on? juce::Colour(0xff69a1d0) : juce::Colour(0xff9da2a8));
        g.setFont(juce::FontOptions(12));
        g.drawText(getButtonText(),r,juce::Justification::centred);
    }
};

class PDAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PDAudioProcessorEditor(PDAudioProcessor&);
    ~PDAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    PDAudioProcessor& p;
    void timerCallback() override;
    void drawRateGraph(juce::Graphics&, juce::Rectangle<float>);
    void refreshCurveButtons(); void refreshTargetButtons(); void refreshShapeButtons();
    void refreshSyncControls();

    juce::TextButton bypassBtn{"BYPASS"};
    juce::TextButton bpmBtn{"BPM"}, hzBtn{"Hz"};

    juce::ComboBox startDivBox, endDivBox;
    juce::Slider startHzSlider, endHzSlider;
    juce::Slider decelTimeSlider, depthSlider;
    juce::Label startRateValueLabel, endRateValueLabel, decelTimeValueLabel, depthValueLabel;

    PDToggleButton expBtn{"EXP"}, linBtn{"LIN"}, logBtn{"LOG"};
    PDToggleButton ampBtn{"AMPLITUDE"}, filterBtn{"FILTER"}, pitchBtn{"PITCH"};
    PDToggleButton sineBtn{"SINE"}, triBtn{"TRIANGLE"}, sqBtn{"SQUARE"}, sawBtn{"SAW"};

    juce::Label triggerLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessorEditor)
};
