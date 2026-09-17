#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class PDToggleButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    void paintButton(juce::Graphics& g, bool over, bool) override {
        auto r=getLocalBounds().toFloat().reduced(0.5f);
        const bool on=getToggleState();
        g.setColour(on?juce::Colour(0xff26394b):juce::Colour(0xff121820));
        g.fillRoundedRectangle(r,7.f);
        g.setColour(on?juce::Colour(0xff78b7e8):(over?juce::Colour(0xff46515d):juce::Colour(0xff303944)));
        g.drawRoundedRectangle(r,7.f,on?1.4f:1.f);
        g.setColour(on?juce::Colour(0xffdceeff):juce::Colour(0xffaeb6bf));
        g.setFont(juce::FontOptions(11.5f).withStyle("bold"));
        g.drawText(getButtonText(),r,juce::Justification::centred);
    }
};

class PDSection : public juce::Component {
public:
    PDSection(const juce::String& title):name(title) {}
    void paint(juce::Graphics& g) override {
        auto r=getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff151d27)); g.fillRoundedRectangle(r,10.f);
        g.setColour(juce::Colour(0xff2c3641)); g.drawRoundedRectangle(r.reduced(.5f),10.f,1.f);
        g.setColour(juce::Colour(0xff8d98a4)); g.setFont(juce::FontOptions(10).withStyle("bold"));
        g.drawText(name,14,9,getWidth()-28,16,juce::Justification::left);
    }
private: juce::String name;
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
    void drawRateGraph(juce::Graphics&,juce::Rectangle<float>);
    void setParam(const char* id,float normalised);
    float paramValue(const char* id) const;
    void refreshFromProcessor();
    void refreshCurveButtons(); void refreshTargetButtons(); void refreshShapeButtons(); void refreshSyncControls();
    static juce::Colour bg(){return juce::Colour(0xff0b1016);} 
    static juce::Colour panel(){return juce::Colour(0xff151d27);} 
    static juce::Colour border(){return juce::Colour(0xff2c3641);} 
    static juce::Colour text(){return juce::Colour(0xffeef3f7);} 
    static juce::Colour muted(){return juce::Colour(0xff8d98a4);} 
    static juce::Colour accent(){return juce::Colour(0xff78b7e8);} 

    juce::TextButton bypassBtn{"BYPASS"};
    PDToggleButton bpmBtn{"BPM"}, hzBtn{"HZ"};
    juce::ComboBox startDivBox,endDivBox;
    juce::Slider startHzSlider,endHzSlider,decelTimeSlider,depthSlider;
    juce::Label startValue,endValue,decelValue,depthValue,triggerValue,inputValue;
    PDToggleButton expBtn{"EXP"},linBtn{"LIN"},logBtn{"LOG"};
    PDToggleButton ampBtn{"AMPLITUDE"},filterBtn{"FILTER"},pitchBtn{"PITCH"};
    PDToggleButton sineBtn{"SINE"},triBtn{"TRIANGLE"},sqBtn{"SQUARE"},sawBtn{"SAW"};
    PDSection rateSection{"RATE"},curveSection{"DECELERATION CURVE"},targetSection{"TARGET"},shapeSection{"SHAPE"};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessorEditor)
};
