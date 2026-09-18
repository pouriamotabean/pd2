#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// FIX (graphics upgrade only - see spec): a small custom LookAndFeel just for the premium button
// depth/press/hover look (sections 11-13, 27, 83 of the spec). Only touches drawButtonBackground/
// drawButtonText - button behaviour (click, toggle state) is entirely unchanged, still plain
// juce::TextButton with setClickingTogglesState(true), still driven by the same onClick handlers.
class PDButtonLookAndFeel : public juce::LookAndFeel_V4 {
public:
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
};

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
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

private:
    PDAudioProcessor& p;
    void timerCallback() override;

    juce::Rectangle<float> patternArea, volumeArea, volumePlotArea, patternGraphArea;
    int draggedPointIndex=-1;
    int hoveredPointIndex=-1; // read in paint() only, never mutates processor/curve state

    // FIX (graphics upgrade): double-click-to-flat visual feedback (spec #36) - which point index to
    // flash, and until what millisecond counter value. Purely cosmetic editor-local state.
    int flashedPointIndex=-1;
    juce::uint32 flashUntilMs=0;

    // Only the Y of each volume-curve control point is drag-editable for now (X positions are fixed
    // anchors defining the "3 preset shapes", per the brief - free X movement is a later feature).
    juce::Point<float> volumePointToScreen(const PDAudioProcessor::VolumeCurve&,int index) const;
    int hitTestVolumePoint(juce::Point<float> screenPos) const;

    PDButtonLookAndFeel pdLnf;

    juce::TextButton bypassBtn{"BYPASS"};
    // FIX (requested): clean, real preset buttons - replaces the old double-click-to-cycle hack, which
    // the person testing it called "awful, laggy". A plain two-button segmented choice is immediate,
    // discoverable, and shows the current state at a glance instead of hiding it behind a gesture.
    juce::TextButton forwardBtn{"FORWARD"}, reverseBtn{"REVERSE"};
    void updatePatternButtonStates();

    // FIX (graphics upgrade - spec #14/#26 assume this exists; it didn't yet): a real 3-way selector
    // for the pVolumeCurve parameter (curve slots 0/1/2, already fully implemented processor-side -
    // see PDAudioProcessor::getVolumeCurveSnapshot/setVolumeCurvePointY/defaultVolumeCurve - there was
    // simply no UI to switch between them before now). Same pattern as forward/reverse: plain
    // TextButtons, setValueNotifyingHost so host automation stays correct.
    juce::TextButton curve1Btn{"CURVE 1"}, curve2Btn{"CURVE 2"}, curve3Btn{"CURVE 3"};
    void updateCurveButtonStates();

    // FIX (graphics upgrade - spec #61/#62): live trigger visualization, computed read-only from the
    // existing safe atomics (uiElapsedSinceTrigger, uiBpm, uiTimeSigNumerator) - no new processor
    // state, no processor logic touched. Converts "time since last trigger" into "fraction of measure
    // since last trigger" so the currently-passing tap can be highlighted as playback moves through
    // the pattern.
    float currentMeasureFraction() const;

    // ---- Colour system (spec section 2 / 70) - centralised, exact palette, no ad-hoc colours ----
    static juce::Colour bg(){return juce::Colour(0xff0d1117);}
    static juce::Colour secondaryBg(){return juce::Colour(0xff10161e);}
    static juce::Colour panel(){return juce::Colour(0xff161e2b);}
    static juce::Colour raised(){return juce::Colour(0xff1d2633);}
    static juce::Colour border(){return juce::Colour(0xff313942);}
    static juce::Colour recess(){return juce::Colour(0xff080b0f);}
    static juce::Colour track(){return juce::Colour(0xff252d36);}
    static juce::Colour hoverShade(){return juce::Colour(0xff3a4148);}
    static juce::Colour text(){return juce::Colour(0xffeef0f2);}
    static juce::Colour secondaryText(){return juce::Colour(0xff9da2a8);}
    static juce::Colour muted(){return juce::Colour(0xff6f7a86);}
    static juce::Colour accent(){return juce::Colour(0xff296095);}
    static juce::Colour accentHighlight(){return juce::Colour(0xff69a1d0);}
    static juce::Colour sidePeak(){return juce::Colour(0xff96c8f2);}

    // ---- Small premium-look drawing helpers (spec section 69) ----
    void drawPremiumPanel(juce::Graphics&, juce::Rectangle<float> bounds, float radius) const;
    void drawRecessedGraph(juce::Graphics&, juce::Rectangle<float> bounds, float radius) const;
    void drawHeader(juce::Graphics&, juce::Rectangle<float> area) const;
    void drawBpmStatus(juce::Graphics&, juce::Rectangle<float> box) const;
    void drawPatternPanel(juce::Graphics&) const;
    void drawPatternGraph(juce::Graphics&, juce::Rectangle<float> tickArea, const PDAudioProcessor::Pattern&) const;
    void drawVolumePanel(juce::Graphics&) const;
    void drawFooter(juce::Graphics&, juce::Rectangle<float> area) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessorEditor)
};
