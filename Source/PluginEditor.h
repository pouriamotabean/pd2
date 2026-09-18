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

// PD - Phase 2 of the Interactive Repeat Pattern Editor rewrite. The Pattern Editor is now the
// primary control surface (spec section 20): click-drag in empty space creates a repeat, dragging a
// repeat's body moves it in time, dragging its top handle edits the value the current EDIT MODE
// represents (VOLUME and PAN are wired up - PITCH/FORMANT are visible as mode buttons but disabled
// until their phases land, per the phased build plan). REVERSE mode's toggle-per-repeat lands later.
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
    // FIX (spec #23 - thread safety): workingPattern is the UI's own editable copy of whichever preset
    // is currently selected. Every mutation is followed by p.commitPattern(...), which lock-free-
    // publishes it to the audio thread (see PDAudioProcessor::commitPattern). The UI never reaches
    // into the processor's internal buffers directly.
    PDAudioProcessor::RepeatPattern workingPattern;
    int selectedEventIndex=-1;
    int hoveredEventIndex=-1;
    enum class DragMode { None, Position, Value };
    DragMode dragMode=DragMode::None;
    int draggedEventIndex=-1;

    // FIX (Phase 2): the vertical axis now represents whichever EDIT MODE is selected (spec #25) -
    // VOLUME or PAN so far. Adding a mode is: one entry in EditMode, one branch in getEditValue/
    // setEditValue/editValueRange, and a button - the drag/hit-test/paint code is entirely generic
    // and doesn't change per mode.
    enum class EditMode { Volume, Pan };
    EditMode currentEditMode=EditMode::Volume;
    float getEditValue(const PDAudioProcessor::RepeatEvent&) const;      // reads the field the current mode represents, in DISPLAY units
    void setEditValue(PDAudioProcessor::RepeatEvent&, float displayValue) const; // writes it back, clamped to that mode's range
    float editValueRangeMax() const; // 12 for dB, 100 for pan - the +/- range of the current mode

    bool isCustomPresetActive() const; // Custom only: position can be moved, repeats can be added/removed
    int currentPresetIndex() const;    // 0=Forward, 1=Reverse, 2=Custom - value editing works on all three
    void refreshWorkingPatternFromProcessor();
    void commitWorkingPattern();

    // Geometry <-> value mapping - X is always time; Y is whatever the current edit mode represents.
    float xToPosition(float x) const;
    float positionToX(float position) const;
    float yToEditValue(float y) const;
    float editValueToY(float displayValue) const;
    juce::Point<float> eventHandlePoint(const PDAudioProcessor::RepeatEvent&) const;

    // Hit testing, in the priority order the spec requires (section 10/22):
    // 1. top handle  2. repeat body  3. empty space
    int hitTestHandle(juce::Point<float> pos) const;
    int hitTestBody(juce::Point<float> pos) const;

    bool snapEnabled=true;
    float snappedPosition(float rawPosition) const;
    float snappedEditValue(float displayValue) const; // FIX (Phase 2): grid/free now applies to the value axis too, not just time

    juce::TextButton bypassBtn{"BYPASS"};
    juce::TextButton forwardBtn{"FORWARD"}, reverseBtn{"REVERSE"}, customBtn{"CUSTOM"};
    void updatePresetButtonStates();

    // Edit-mode buttons (spec section 2). VOLUME and PAN are functional (Phase 1/2) - the remaining
    // three are visible (so the final layout is already in place) but disabled until their phases land.
    juce::TextButton volumeModeBtn{"VOLUME"}, panModeBtn{"PAN"}, pitchModeBtn{"PITCH"},
                     formantModeBtn{"FORMANT"}, reverseModeBtn{"REVERSE"};
    void updateModeButtonStates();
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
