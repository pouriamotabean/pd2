#include "PluginEditor.h"

namespace {
juce::Colour bg(){return juce::Colour(0xff0d1117);}
juce::Colour panel(){return juce::Colour(0xff161e2b);}
juce::Colour raised(){return juce::Colour(0xff1d2633);}
juce::Colour border(){return juce::Colour(0xff313942);}
juce::Colour textPrimary(){return juce::Colour(0xffeef0f2);}
juce::Colour textSecondary(){return juce::Colour(0xff9da2a8);}
juce::Colour textMuted(){return juce::Colour(0xff6f7a86);}
juce::Colour accent(){return juce::Colour(0xff69a1d0);}

void styleKnob(juce::Slider& s) {
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
    s.setColour(juce::Slider::rotarySliderFillColourId, accent());
    s.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff252d36));
    s.setColour(juce::Slider::thumbColourId, textPrimary());
}
void styleLabelValue(juce::Label& l) {
    l.setJustificationType(juce::Justification::centred);
    l.setColour(juce::Label::textColourId, textPrimary());
    l.setFont(juce::FontOptions(13));
}
}

PDAudioProcessorEditor::PDAudioProcessorEditor(PDAudioProcessor& proc)
    : juce::AudioProcessorEditor(&proc), p(proc)
{
    setSize(700,480);

    addAndMakeVisible(bypassBtn);
    bypassBtn.setClickingTogglesState(true);
    bypassBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff10161e));
    bypassBtn.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff69a1d0));
    bypassBtn.setColour(juce::TextButton::textColourOffId, textPrimary());
    bypassBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    bypassBtn.onClick=[this]{ p.bypassed.store(bypassBtn.getToggleState()); };

    addAndMakeVisible(bpmBtn); addAndMakeVisible(hzBtn);
    for (auto* b : {&bpmBtn,&hzBtn}) {
        b->setClickingTogglesState(true);
        b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff10161e));
        b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff69a1d0));
        b->setColour(juce::TextButton::textColourOffId, textSecondary());
        b->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    }
    bpmBtn.onClick=[this]{ p.bpmSync.store(true); refreshSyncControls(); };
    hzBtn.onClick=[this]{ p.bpmSync.store(false); refreshSyncControls(); };

    for (auto* box : {&startDivBox,&endDivBox}) {
        addAndMakeVisible(*box);
        box->setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff10161e));
        box->setColour(juce::ComboBox::outlineColourId, border());
        box->setColour(juce::ComboBox::textColourId, textPrimary());
        box->setColour(juce::ComboBox::arrowColourId, textSecondary());
        for (int i=0;i<PDAudioProcessor::kNumDivisions;++i)
            box->addItem(PDAudioProcessor::kDivisionNames[i], i+1);
    }
    startDivBox.setSelectedId(p.startDivIndex.load()+1, juce::dontSendNotification);
    endDivBox.setSelectedId(p.endDivIndex.load()+1, juce::dontSendNotification);
    startDivBox.onChange=[this]{ p.startDivIndex.store(startDivBox.getSelectedId()-1); };
    endDivBox.onChange=[this]{ p.endDivIndex.store(endDivBox.getSelectedId()-1); };

    styleKnob(startHzSlider); styleKnob(endHzSlider); styleKnob(decelTimeSlider); styleKnob(depthSlider);
    addAndMakeVisible(startHzSlider); addAndMakeVisible(endHzSlider);
    addAndMakeVisible(decelTimeSlider); addAndMakeVisible(depthSlider);
    startHzSlider.setRange(0.5,20.0,0.01); startHzSlider.setValue(p.startRateHz.load());
    endHzSlider.setRange(0.5,20.0,0.01); endHzSlider.setValue(p.endRateHz.load());
    decelTimeSlider.setRange(0.1,10.0,0.01); decelTimeSlider.setValue(p.decelTimeSec.load());
    depthSlider.setRange(0.0,100.0,0.1); depthSlider.setValue(p.depth.load()*100.0);
    startHzSlider.onValueChange=[this]{ p.startRateHz.store((float)startHzSlider.getValue()); startRateValueLabel.setText(juce::String(startHzSlider.getValue(),2)+" Hz",juce::dontSendNotification); };
    endHzSlider.onValueChange=[this]{ p.endRateHz.store((float)endHzSlider.getValue()); endRateValueLabel.setText(juce::String(endHzSlider.getValue(),2)+" Hz",juce::dontSendNotification); };
    decelTimeSlider.onValueChange=[this]{ p.decelTimeSec.store((float)decelTimeSlider.getValue()); decelTimeValueLabel.setText(juce::String(decelTimeSlider.getValue(),2)+" s",juce::dontSendNotification); };
    depthSlider.onValueChange=[this]{ p.depth.store((float)depthSlider.getValue()/100.f); depthValueLabel.setText(juce::String((int)depthSlider.getValue())+" %",juce::dontSendNotification); };

    for (auto* l : {&startRateValueLabel,&endRateValueLabel,&decelTimeValueLabel,&depthValueLabel}) { styleLabelValue(*l); addAndMakeVisible(*l); }
    decelTimeValueLabel.setText(juce::String(decelTimeSlider.getValue(),2)+" s",juce::dontSendNotification);
    depthValueLabel.setText(juce::String((int)depthSlider.getValue())+" %",juce::dontSendNotification);

    for (auto* b : {&expBtn,&linBtn,&logBtn}) addAndMakeVisible(*b);
    expBtn.onClick=[this]{ p.decelCurve.store(PDAudioProcessor::DecelCurve::Exponential); refreshCurveButtons(); };
    linBtn.onClick=[this]{ p.decelCurve.store(PDAudioProcessor::DecelCurve::Linear); refreshCurveButtons(); };
    logBtn.onClick=[this]{ p.decelCurve.store(PDAudioProcessor::DecelCurve::Logarithmic); refreshCurveButtons(); };

    for (auto* b : {&ampBtn,&filterBtn,&pitchBtn}) addAndMakeVisible(*b);
    ampBtn.onClick=[this]{ p.target.store(PDAudioProcessor::Target::Amplitude); refreshTargetButtons(); };
    filterBtn.onClick=[this]{ p.target.store(PDAudioProcessor::Target::Filter); refreshTargetButtons(); };
    pitchBtn.onClick=[this]{ p.target.store(PDAudioProcessor::Target::Pitch); refreshTargetButtons(); };

    for (auto* b : {&sineBtn,&triBtn,&sqBtn,&sawBtn}) addAndMakeVisible(*b);
    sineBtn.onClick=[this]{ p.shape.store(PDAudioProcessor::Shape::Sine); refreshShapeButtons(); };
    triBtn.onClick=[this]{ p.shape.store(PDAudioProcessor::Shape::Triangle); refreshShapeButtons(); };
    sqBtn.onClick=[this]{ p.shape.store(PDAudioProcessor::Shape::Square); refreshShapeButtons(); };
    sawBtn.onClick=[this]{ p.shape.store(PDAudioProcessor::Shape::Saw); refreshShapeButtons(); };

    addAndMakeVisible(triggerLabel);
    triggerLabel.setJustificationType(juce::Justification::centredLeft);
    triggerLabel.setColour(juce::Label::textColourId, textSecondary());
    triggerLabel.setFont(juce::FontOptions(12));

    refreshCurveButtons(); refreshTargetButtons(); refreshShapeButtons(); refreshSyncControls();
    startTimerHz(30);
}

PDAudioProcessorEditor::~PDAudioProcessorEditor() { stopTimer(); }

void PDAudioProcessorEditor::refreshCurveButtons(){
    auto c=p.decelCurve.load();
    expBtn.setToggleState(c==PDAudioProcessor::DecelCurve::Exponential,juce::dontSendNotification);
    linBtn.setToggleState(c==PDAudioProcessor::DecelCurve::Linear,juce::dontSendNotification);
    logBtn.setToggleState(c==PDAudioProcessor::DecelCurve::Logarithmic,juce::dontSendNotification);
}
void PDAudioProcessorEditor::refreshTargetButtons(){
    auto t=p.target.load();
    ampBtn.setToggleState(t==PDAudioProcessor::Target::Amplitude,juce::dontSendNotification);
    filterBtn.setToggleState(t==PDAudioProcessor::Target::Filter,juce::dontSendNotification);
    pitchBtn.setToggleState(t==PDAudioProcessor::Target::Pitch,juce::dontSendNotification);
}
void PDAudioProcessorEditor::refreshShapeButtons(){
    auto s=p.shape.load();
    sineBtn.setToggleState(s==PDAudioProcessor::Shape::Sine,juce::dontSendNotification);
    triBtn.setToggleState(s==PDAudioProcessor::Shape::Triangle,juce::dontSendNotification);
    sqBtn.setToggleState(s==PDAudioProcessor::Shape::Square,juce::dontSendNotification);
    sawBtn.setToggleState(s==PDAudioProcessor::Shape::Saw,juce::dontSendNotification);
}
void PDAudioProcessorEditor::refreshSyncControls(){
    bool sync=p.bpmSync.load();
    bpmBtn.setToggleState(sync,juce::dontSendNotification);
    hzBtn.setToggleState(!sync,juce::dontSendNotification);
    startDivBox.setVisible(sync); endDivBox.setVisible(sync);
    startHzSlider.setVisible(!sync); endHzSlider.setVisible(!sync);
    startRateValueLabel.setVisible(!sync); endRateValueLabel.setVisible(!sync);
}

void PDAudioProcessorEditor::timerCallback(){
    triggerLabel.setText(juce::String(p.uiElapsedSinceTrigger.load(),2)+" s ago",juce::dontSendNotification);
    repaint();
}

void PDAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(bg());
    auto a=getLocalBounds().toFloat();

    // Header
    g.setColour(accent()); g.setFont(juce::FontOptions(24).withStyle("bold"));
    g.drawText("[PD]", 24,16,120,28, juce::Justification::left);
    g.setColour(textMuted()); g.setFont(juce::FontOptions(12));
    g.drawText("DECELERATING MODULATOR", 150,20,260,20, juce::Justification::left);

    // Graph panel
    auto graph=juce::Rectangle<float>(24.f,64.f,a.getWidth()-48.f,150.f);
    g.setColour(panel()); g.fillRoundedRectangle(graph,10.f);
    g.setColour(border()); g.drawRoundedRectangle(graph.reduced(0.5f),10.f,1.f);
    g.setColour(accent()); g.setFont(juce::FontOptions(11));
    g.drawText("RATE OVER TIME", graph.getX()+12,graph.getY()+8,150,14, juce::Justification::left);
    drawRateGraph(g, graph.reduced(14.f).withTrimmedTop(20.f));

    // Knob section labels
    g.setColour(textSecondary()); g.setFont(juce::FontOptions(11));
    struct KnobLbl{const char* t; float x;};
    for (auto& kl : std::array<KnobLbl,4>{{{"START RATE",40.f},{"END RATE",190.f},{"DECELERATION TIME",340.f},{"DEPTH",560.f}}})
        g.drawText(kl.t, kl.x,232.f,140.f,14.f, juce::Justification::left);
    g.drawText("CURVE", 480.f,232.f,100.f,14.f, juce::Justification::left);

    // Bottom section labels
    g.drawText("TARGET", 24.f,352.f,100.f,14.f, juce::Justification::left);
    g.drawText("SHAPE", 340.f,352.f,100.f,14.f, juce::Justification::left);

    // Trigger indicator dot, next to the trigger label inside the graph panel's top-right corner
    bool recent = p.uiElapsedSinceTrigger.load() < 1.5f;
    g.setColour(recent? accent() : textMuted());
    g.fillEllipse((float)(getWidth()-24-158),90.f,10.f,10.f);
}

void PDAudioProcessorEditor::drawRateGraph(juce::Graphics& g, juce::Rectangle<float> r) {
    g.setColour(textMuted()); g.setFont(juce::FontOptions(9));
    g.drawText("FAST", r.getX()-2,r.getY()-4,40,12, juce::Justification::left);
    g.drawText("SLOW", r.getX()-2,r.getBottom()-10,40,12, juce::Justification::left);
    g.setColour(border().withAlpha(0.4f));
    for (int i=1;i<4;++i) { float gx=r.getX()+r.getWidth()*i/4.f; g.drawVerticalLine((int)gx,r.getY(),r.getBottom()); }

    float startHz,endHz;
    if (p.bpmSync.load()) {
        int si=juce::jlimit(0,PDAudioProcessor::kNumDivisions-1,p.startDivIndex.load());
        int ei=juce::jlimit(0,PDAudioProcessor::kNumDivisions-1,p.endDivIndex.load());
        startHz=(float)(p.currentBpm/(60.0*PDAudioProcessor::kDivisionBeats[si]));
        endHz  =(float)(p.currentBpm/(60.0*PDAudioProcessor::kDivisionBeats[ei]));
    } else { startHz=p.startRateHz.load(); endHz=p.endRateHz.load(); }
    float hi=juce::jmax(startHz,endHz), lo=juce::jmin(startHz,endHz);
    auto curve=p.decelCurve.load();

    juce::Path path, fillPath;
    const int N=100;
    for (int i=0;i<=N;++i) {
        float t=(float)i/N;
        float shaped=PDAudioProcessor::shapeCurve(t,curve);
        float rate=startHz+(endHz-startHz)*shaped;
        float norm = (hi>lo)? (rate-lo)/(hi-lo) : 0.5f;
        float x=r.getX()+r.getWidth()*t, y=r.getBottom()-r.getHeight()*norm;
        if (i==0) { path.startNewSubPath(x,y); fillPath.startNewSubPath(x,r.getBottom()); fillPath.lineTo(x,y); }
        else { path.lineTo(x,y); fillPath.lineTo(x,y); }
    }
    fillPath.lineTo(r.getRight(),r.getBottom()); fillPath.closeSubPath();
    g.setColour(accent().withAlpha(0.12f)); g.fillPath(fillPath);
    g.setColour(accent()); g.strokePath(path, juce::PathStrokeType(2.f));
}

void PDAudioProcessorEditor::resized() {
    bypassBtn.setBounds(getWidth()-190,18,90,28);
    bpmBtn.setBounds(getWidth()-92,18,44,28);
    hzBtn.setBounds(getWidth()-48,18,34,28);

    int knobY=250, knobSize=64;
    startHzSlider.setBounds(40,knobY,knobSize,knobSize);
    startDivBox.setBounds(30,knobY+16,110,30);
    startRateValueLabel.setBounds(30,knobY+knobSize+4,110,16);

    endHzSlider.setBounds(190,knobY,knobSize,knobSize);
    endDivBox.setBounds(180,knobY+16,110,30);
    endRateValueLabel.setBounds(180,knobY+knobSize+4,110,16);

    decelTimeSlider.setBounds(345,knobY,knobSize,knobSize);
    decelTimeValueLabel.setBounds(330,knobY+knobSize+4,110,16);

    int cx=480, cy=250;
    expBtn.setBounds(cx,cy,60,26); linBtn.setBounds(cx,cy+30,60,26); logBtn.setBounds(cx,cy+60,60,26);

    depthSlider.setBounds(575,knobY,knobSize,knobSize);
    depthValueLabel.setBounds(560,knobY+knobSize+4,110,16);

    ampBtn.setBounds(24,370,110,30);
    filterBtn.setBounds(140,370,90,30);
    pitchBtn.setBounds(236,370,80,30);

    sineBtn.setBounds(340,370,70,30);
    triBtn.setBounds(416,370,90,30);
    sqBtn.setBounds(512,370,90,30);
    sawBtn.setBounds(608,370,60,30);

    triggerLabel.setBounds((int)(getWidth()-24-140),86,130,20);
}
