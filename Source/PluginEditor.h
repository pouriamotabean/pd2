#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// FIX (graphics upgrade, unchanged from before): a small custom LookAndFeel just for the premium
// button depth/press/hover look. Only touches drawing - button behaviour is plain juce::TextButton.
class PDButtonLookAndFeel : public juce::LookAndFeel_V4 {
public:
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
};

// PD - Phase 1 of the Interactive Repeat Pattern Editor rewrite. The Pattern Editor is now the
// primary control surface (spec section 20): click-drag in empty space creates a repeat, dragging a
// repeat's body moves it in time, dragging its top handle edits the value the current EDIT MODE
// represents (Phase 1: VOLUME only - PAN/PITCH/FORMANT are visible as mode buttons but disabled until
// their phases land, per the phased build plan). REVERSE mode's toggle-per-repeat also lands later.
class PDAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PDAudioProcessorEditor(PDAudioProcessor&);
    ~PDAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    PDAudioProcessor& p;
    void timerCallback() override;

    juce::Rectangle<float> headerArea, editorPanelArea, graphArea;

    // ---- Editing state (spec sections 9/10/12/22) ----
    // FIX (spec #23 - thread safety): workingPattern is the UI's own editable copy. Every mutation is
    // followed by p.commitCustomPattern(workingPattern), which lock-free-publishes it to the audio
    // thread (see PDAudioProcessor::commitCustomPattern). The UI never reaches into the processor's
    // internal buffers directly.
    PDAudioProcessor::RepeatPattern workingPattern;
    int selectedEventIndex=-1;
    int hoveredEventIndex=-1;
    enum class DragMode { None, Position, Value };
    DragMode dragMode=DragMode::None;
    int draggedEventIndex=-1;

    bool isCustomPresetActive() const;
    void refreshWorkingPatternFromProcessor();
    void commitWorkingPattern();

    // Geometry <-> value mapping for the current edit mode (Phase 1: VOLUME only)
    float xToPosition(float x) const;
    float positionToX(float position) const;
    float yToVolumeDb(float y) const;
    float volumeDbToY(float db) const;
    juce::Point<float> eventHandlePoint(const PDAudioProcessor::RepeatEvent&) const;

    // Hit testing, in the priority order the spec requires (section 10/22):
    // 1. top handle  2. repeat body  3. empty space
    int hitTestHandle(juce::Point<float> pos) const;
    int hitTestBody(juce::Point<float> pos) const;

    bool snapEnabled=true;
    float snappedPosition(float rawPosition) const;

    juce::TextButton bypassBtn{"BYPASS"};
    juce::TextButton forwardBtn{"FORWARD"}, reverseBtn{"REVERSE"}, customBtn{"CUSTOM"};
    void updatePresetButtonStates();

    // Edit-mode buttons (spec section 2). Only VOLUME is functional in Phase 1 - the other four are
    // visible (so the final layout is already in place) but disabled until their phases land.
    juce::TextButton volumeModeBtn{"VOLUME"}, panModeBtn{"PAN"}, pitchModeBtn{"PITCH"},
                     formantModeBtn{"FORMANT"}, reverseModeBtn{"REVERSE"};
    juce::TextButton snapBtn{"SNAP"};

    PDButtonLookAndFeel pdLnf;

    static juce::Colour bg(){return juce::Colour(0xff0d1117);}
    static juce::Colour secondaryBg(){return juce::Colour(0xff10161e);}
    static juce::Colour panel(){return juce::Colour(0xff161e2b);}
    static juce::Colour raised(){return juce::Colour(0xff1d2633);}
    static juce::Colour border(){return juce::Colour(0xff313942);}
    static juce::Colour recess(){return juce::Colour(0xff080b0f);}
    static juce::Colour text(){return juce::Colour(0xffeef0f2);}
    static juce::Colour secondaryText(){return juce::Colour(0xff9da2a8);}
    static juce::Colour muted(){return juce::Colour(0xff6f7a86);}
    static juce::Colour accent(){return juce::Colour(0xff296095);}
    static juce::Colour accentHighlight(){return juce::Colour(0xff69a1d0);}
    static juce::Colour sidePeak(){return juce::Colour(0xff96c8f2);}

    void drawPremiumPanel(juce::Graphics&, juce::Rectangle<float> bounds, float radius) const;
    void drawHeader(juce::Graphics&) const;
    void drawBpmStatus(juce::Graphics&, juce::Rectangle<float> box) const;
    void drawEditorPanel(juce::Graphics&) const;
    void drawGraph(juce::Graphics&) const;
    void drawFooter(juce::Graphics&, juce::Rectangle<float> area) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessorEditor)
};
