#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class PDAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PDAudioProcessorEditor(PDAudioProcessor&);
    ~PDAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    PDAudioProcessor& p;
    void timerCallback() override;

    juce::Rectangle<float> patternArea, volumeArea, volumePlotArea, patternLabelHitBox;
    int draggedPointIndex=-1;

    // Only the Y of each volume-curve control point is drag-editable for now (X positions are fixed
    // anchors defining the "3 preset shapes", per the brief - free X movement is a later feature).
    juce::Point<float> volumePointToScreen(const PDAudioProcessor::VolumeCurve&,int index) const;
    int hitTestVolumePoint(juce::Point<float> screenPos) const;

    juce::TextButton bypassBtn{"BYPASS"};

    static juce::Colour bg(){return juce::Colour(0xff0b1016);}
    static juce::Colour panel(){return juce::Colour(0xff151d27);}
    static juce::Colour border(){return juce::Colour(0xff2c3641);}
    static juce::Colour text(){return juce::Colour(0xffeef3f7);}
    static juce::Colour muted(){return juce::Colour(0xff8d98a4);}
    static juce::Colour accent(){return juce::Colour(0xff78b7e8);}

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessorEditor)
};
