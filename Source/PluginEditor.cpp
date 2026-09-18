#include "PluginEditor.h"

// =====================================================================================
// PDButtonLookAndFeel - premium button depth (unchanged from the graphics-upgrade pass)
// =====================================================================================
void PDButtonLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                bool isHighlighted, bool isDown){
    auto bounds = button.getLocalBounds().toFloat();
    const bool active = button.getToggleState();
    const float radius = 6.0f;

    g.setColour(juce::Colour(0xff080b0f));
    g.fillRoundedRectangle(bounds, radius);

    auto face = bounds.reduced(0.5f).withTrimmedBottom(isDown?0.5f:2.0f);
    if(isDown) face = face.translated(0.f, 1.5f);

    juce::Colour faceColour = active ? juce::Colour(0xff1d2633) : juce::Colour(0xff10161e);
    if(!button.isEnabled()) faceColour = juce::Colour(0xff10161e).withAlpha(0.5f);
    else if(isHighlighted && !isDown) faceColour = faceColour.interpolatedWith(juce::Colour(0xff3a4148), 0.30f);
    g.setColour(faceColour);
    g.fillRoundedRectangle(face, radius);

    float highlightAlpha = isDown ? 0.08f : (active ? 0.30f : 0.18f);
    juce::Path topHighlight;
    topHighlight.addRoundedRectangle(face.getX(), face.getY(), face.getWidth(), juce::jmax(2.0f,face.getHeight()*0.45f),
                                      radius, radius, true, true, false, false);
    g.setColour(juce::Colour(0xff313942).withAlpha(button.isEnabled()?highlightAlpha:highlightAlpha*0.4f));
    g.fillPath(topHighlight);

    juce::Colour borderColour = active ? juce::Colour(0xff296095) : juce::Colour(0xff313942);
    if(!button.isEnabled()) borderColour = borderColour.withAlpha(0.4f);
    else if(isHighlighted && !active) borderColour = borderColour.brighter(0.2f);
    g.setColour(borderColour);
    g.drawRoundedRectangle(face, radius, active?1.4f:1.0f);
}

void PDButtonLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                          bool isHighlighted, bool isDown){
    const bool active = button.getToggleState();
    juce::Colour col = active ? juce::Colour(0xff69a1d0) : juce::Colour(0xff6f7a86);
    if(!button.isEnabled()) col = col.withAlpha(0.35f);
    else if(isHighlighted && !active) col = col.brighter(0.25f);
    g.setColour(col);
    g.setFont(juce::FontOptions(10.5f).withStyle("bold"));
    auto bounds = button.getLocalBounds();
    if(isDown) bounds = bounds.translated(0, 1);
    g.drawText(button.getButtonText(), bounds, juce::Justification::centred);
}

// =====================================================================================
// PDAudioProcessorEditor
// =====================================================================================
PDAudioProcessorEditor::PDAudioProcessorEditor(PDAudioProcessor& proc):AudioProcessorEditor(&proc),p(proc){
    setSize(1260,650);
    setWantsKeyboardFocus(true);

    for(auto* btn:{&bypassBtn,&forwardBtn,&reverseBtn,&customBtn,
                   &volumeModeBtn,&panModeBtn,&pitchModeBtn,&formantModeBtn,&reverseModeBtn,&snapBtn}){
        addAndMakeVisible(*btn);
        btn->setClickingTogglesState(true);
        btn->setLookAndFeel(&pdLnf);
    }

    bypassBtn.onClick=[this]{
        if(auto* a=p.apvts.getParameter(PDAudioProcessor::pBypass))
            a->setValueNotifyingHost(bypassBtn.getToggleState()?1.f:0.f);
    };

    // Preset selector: Forward / Reverse (locked, unchanged shapes) / Custom (the new editable one)
    forwardBtn.onClick=[this]{ if(auto* a=p.apvts.getParameter(PDAudioProcessor::pPreset)) a->setValueNotifyingHost(0.f/2.f); updatePresetButtonStates(); refreshWorkingPatternFromProcessor(); selectedEventIndex=-1; };
    reverseBtn.onClick=[this]{ if(auto* a=p.apvts.getParameter(PDAudioProcessor::pPreset)) a->setValueNotifyingHost(1.f/2.f); updatePresetButtonStates(); refreshWorkingPatternFromProcessor(); selectedEventIndex=-1; };
    customBtn.onClick=[this]{ if(auto* a=p.apvts.getParameter(PDAudioProcessor::pPreset)) a->setValueNotifyingHost(2.f/2.f); updatePresetButtonStates(); refreshWorkingPatternFromProcessor(); selectedEventIndex=-1; };
    updatePresetButtonStates();

    // Edit-mode buttons (spec section 2) - VOLUME and PAN are wired up (Phase 1/2). The remaining
    // three are shown (so the final 5-button layout already exists) but disabled until their phases
    // land, with a tooltip saying so rather than silently doing nothing.
    volumeModeBtn.onClick=[this]{ currentEditMode=EditMode::Volume; updateModeButtonStates(); repaint(); };
    panModeBtn.onClick=[this]{ currentEditMode=EditMode::Pan; updateModeButtonStates(); repaint(); };
    updateModeButtonStates();
    for(auto* btn:{&pitchModeBtn,&formantModeBtn,&reverseModeBtn}){
        btn->setEnabled(false);
        btn->setTooltip("Coming in a later phase - see the phased build plan.");
    }

    snapBtn.setToggleState(snapEnabled,juce::dontSendNotification);
    snapBtn.setTooltip("Snap repeat position to the beat grid while dragging.");
    snapBtn.onClick=[this]{ snapEnabled=snapBtn.getToggleState(); };

    refreshWorkingPatternFromProcessor();
    startTimerHz(30);
}
PDAudioProcessorEditor::~PDAudioProcessorEditor(){
    stopTimer();
    for(auto* btn:{&bypassBtn,&forwardBtn,&reverseBtn,&customBtn,
                   &volumeModeBtn,&panModeBtn,&pitchModeBtn,&formantModeBtn,&reverseModeBtn,&snapBtn})
        btn->setLookAndFeel(nullptr);
}

bool PDAudioProcessorEditor::isCustomPresetActive() const {
    return currentPresetIndex()==(int)PDAudioProcessor::kPresetCustom;
}
int PDAudioProcessorEditor::currentPresetIndex() const {
    return (int)p.apvts.getRawParameterValue(PDAudioProcessor::pPreset)->load();
}
void PDAudioProcessorEditor::refreshWorkingPatternFromProcessor(){
    workingPattern = p.getPatternForEditing(currentPresetIndex());
}
void PDAudioProcessorEditor::commitWorkingPattern(){
    p.commitPattern(currentPresetIndex(),workingPattern);
}

void PDAudioProcessorEditor::updatePresetButtonStates(){
    int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pPreset)->load();
    forwardBtn.setToggleState(idx==(int)PDAudioProcessor::kPresetForward,juce::dontSendNotification);
    reverseBtn.setToggleState(idx==(int)PDAudioProcessor::kPresetReverse,juce::dontSendNotification);
    customBtn.setToggleState(idx==(int)PDAudioProcessor::kPresetCustom,juce::dontSendNotification);
}
void PDAudioProcessorEditor::updateModeButtonStates(){
    volumeModeBtn.setToggleState(currentEditMode==EditMode::Volume,juce::dontSendNotification);
    panModeBtn.setToggleState(currentEditMode==EditMode::Pan,juce::dontSendNotification);
}

// ---- Edit-mode value access (spec section 2/25) --------------------------------------------
float PDAudioProcessorEditor::getEditValue(const PDAudioProcessor::RepeatEvent& e) const {
    switch(currentEditMode){
        case EditMode::Volume: return e.volumeDb;
        case EditMode::Pan: return e.pan*100.f; // display in -100..+100, per spec section 5
    }
    return 0.f;
}
void PDAudioProcessorEditor::setEditValue(PDAudioProcessor::RepeatEvent& e, float displayValue) const {
    switch(currentEditMode){
        case EditMode::Volume: e.volumeDb=juce::jlimit(-12.f,12.f,displayValue); break;
        case EditMode::Pan: e.pan=juce::jlimit(-1.f,1.f,displayValue*0.01f); break;
    }
}
float PDAudioProcessorEditor::editValueRangeMax() const {
    return currentEditMode==EditMode::Volume ? 12.f : 100.f;
}

// ---- Geometry <-> value mapping - X is always time; Y is whatever the edit mode represents ----
float PDAudioProcessorEditor::xToPosition(float x) const {
    if(graphArea.getWidth()<=0.f) return 0.f;
    return juce::jlimit(0.f,1.f,(x-graphArea.getX())/graphArea.getWidth());
}
float PDAudioProcessorEditor::positionToX(float position) const {
    return graphArea.getX()+graphArea.getWidth()*juce::jlimit(0.f,1.f,position);
}
float PDAudioProcessorEditor::yToEditValue(float y) const {
    float halfH=graphArea.getHeight()*0.5f;
    if(halfH<=0.f) return 0.f;
    float range=editValueRangeMax();
    float v = -(y-graphArea.getCentreY())/halfH*range;
    return juce::jlimit(-range,range,v);
}
float PDAudioProcessorEditor::editValueToY(float displayValue) const {
    float halfH=graphArea.getHeight()*0.5f;
    float range=editValueRangeMax();
    return graphArea.getCentreY() - (juce::jlimit(-range,range,displayValue)/range)*halfH;
}
juce::Point<float> PDAudioProcessorEditor::eventHandlePoint(const PDAudioProcessor::RepeatEvent& e) const {
    return { positionToX(e.position), editValueToY(getEditValue(e)) };
}
float PDAudioProcessorEditor::snappedPosition(float rawPosition) const {
    if(!snapEnabled) return rawPosition;
    int numBeats=juce::jmax(1,p.uiTimeSigNumerator.load());
    int divisions=numBeats*4; // spec section 19: a subtle 1/16-of-a-bar grid
    float step=1.0f/(float)divisions;
    return juce::jlimit(0.f,1.f,std::round(rawPosition/step)*step);
}
float PDAudioProcessorEditor::snappedEditValue(float displayValue) const {
    // FIX (requested - grid/free applies to values too, not just time): VOLUME snaps to whole dB
    // steps, PAN snaps to 10% steps (L100..C..R100 in tidy increments) - same SNAP toggle as position.
    if(!snapEnabled) return displayValue;
    float step = currentEditMode==EditMode::Volume ? 1.0f : 10.0f;
    return std::round(displayValue/step)*step;
}

int PDAudioProcessorEditor::hitTestHandle(juce::Point<float> pos) const {
    for(int i=0;i<workingPattern.count;++i){
        if(eventHandlePoint(workingPattern.events[i]).getDistanceFrom(pos)<11.f) return i;
    }
    return -1;
}
int PDAudioProcessorEditor::hitTestBody(juce::Point<float> pos) const {
    if(!graphArea.contains(pos)) return -1;
    for(int i=0;i<workingPattern.count;++i){
        float x=positionToX(workingPattern.events[i].position);
        if(std::abs(pos.x-x)>7.f) continue;
        float y0=graphArea.getCentreY(), y1=editValueToY(getEditValue(workingPattern.events[i]));
        float top=juce::jmin(y0,y1)-4.f, bottom=juce::jmax(y0,y1)+4.f;
        if(pos.y>=top && pos.y<=bottom) return i;
    }
    return -1;
}

void PDAudioProcessorEditor::mouseDown(const juce::MouseEvent& e){
    grabKeyboardFocus();
    if(!graphArea.expanded(14.f).contains(e.position)) return;
    refreshWorkingPatternFromProcessor();

    // Interaction priority (spec #10/#22): handle before body before empty space. The handle (value
    // editing) works on ALL THREE presets now - only body-drag (reposition) and empty-space creation
    // are Custom-only, since Forward/Reverse's positions are fixed by design.
    int h=hitTestHandle(e.position);
    if(h>=0){ draggedEventIndex=h; dragMode=DragMode::Value; selectedEventIndex=h; repaint(); return; }

    if(!isCustomPresetActive()) return; // Forward/Reverse: no repositioning, no creating/removing repeats

    int bIdx=hitTestBody(e.position);
    if(bIdx>=0){ draggedEventIndex=bIdx; dragMode=DragMode::Position; selectedEventIndex=bIdx; repaint(); return; }

    // Empty space -> create a new Repeat (spec section 9) - Custom only
    if(workingPattern.count>=PDAudioProcessor::kMaxRepeats) return; // full - ignore rather than overflow
    int idx=workingPattern.count++;
    auto& ev=workingPattern.events[idx];
    ev=PDAudioProcessor::RepeatEvent{};
    ev.id=idx;
    ev.position=snappedPosition(xToPosition(e.position.x));
    setEditValue(ev,snappedEditValue(yToEditValue(e.position.y)));
    draggedEventIndex=idx; dragMode=DragMode::Value; selectedEventIndex=idx;
    commitWorkingPattern();
    repaint();
}
void PDAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e){
    if(draggedEventIndex<0 || draggedEventIndex>=workingPattern.count) return;
    auto& ev=workingPattern.events[draggedEventIndex];
    if(dragMode==DragMode::Value) setEditValue(ev,snappedEditValue(yToEditValue(e.position.y)));
    else if(dragMode==DragMode::Position && isCustomPresetActive()) ev.position=snappedPosition(xToPosition(e.position.x));
    commitWorkingPattern();
    repaint();
}
void PDAudioProcessorEditor::mouseUp(const juce::MouseEvent&){
    draggedEventIndex=-1; dragMode=DragMode::None;
}
void PDAudioProcessorEditor::mouseMove(const juce::MouseEvent& e){
    int h=hitTestHandle(e.position); // value-edit handles are hoverable on all three presets now
    if(h!=hoveredEventIndex){ hoveredEventIndex=h; repaint(); }
    if(h>=0) setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    else if(!isCustomPresetActive()) setMouseCursor(juce::MouseCursor::NormalCursor);
    else if(hitTestBody(e.position)>=0) setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    else if(graphArea.contains(e.position)) setMouseCursor(juce::MouseCursor::CrosshairCursor);
    else setMouseCursor(juce::MouseCursor::NormalCursor);
}
void PDAudioProcessorEditor::mouseExit(const juce::MouseEvent&){
    if(hoveredEventIndex!=-1){ hoveredEventIndex=-1; repaint(); }
}
bool PDAudioProcessorEditor::keyPressed(const juce::KeyPress& k){
    if((k==juce::KeyPress::deleteKey || k==juce::KeyPress::backspaceKey)
        && isCustomPresetActive() && selectedEventIndex>=0 && selectedEventIndex<workingPattern.count){
        for(int i=selectedEventIndex;i<workingPattern.count-1;++i) workingPattern.events[i]=workingPattern.events[i+1];
        --workingPattern.count;
        selectedEventIndex=-1; draggedEventIndex=-1; dragMode=DragMode::None;
        commitWorkingPattern();
        repaint();
        return true;
    }
    return false;
}

void PDAudioProcessorEditor::timerCallback(){
    updatePresetButtonStates();
    if(dragMode==DragMode::None) refreshWorkingPatternFromProcessor(); // stay in sync without clobbering a live drag
    repaint();
}

// ---- Drawing --------------------------------------------------------------------------------
void PDAudioProcessorEditor::drawPremiumPanel(juce::Graphics& g, juce::Rectangle<float> bounds, float radius) const {
    g.setColour(recess()); g.fillRoundedRectangle(bounds.expanded(1.0f), radius);
    g.setColour(panel()); g.fillRoundedRectangle(bounds, radius);
    juce::Path topHighlight;
    topHighlight.addRoundedRectangle(bounds.getX(),bounds.getY(),bounds.getWidth(),bounds.getHeight()*0.5f,
                                      radius,radius,true,true,false,false);
    g.setColour(raised().withAlpha(0.18f)); g.fillPath(topHighlight);
    g.setColour(border()); g.drawRoundedRectangle(bounds,radius,1.0f);
}

void PDAudioProcessorEditor::drawHeader(juce::Graphics& g) const {
    g.setColour(text()); g.setFont(juce::FontOptions(31.f).withStyle("bold"));
    g.drawText("PD",28.f,18.f,90.f,38.f,juce::Justification::left);
    g.setColour(accent().withAlpha(0.7f));
    g.fillRect(122.f,24.f,1.6f,32.f);
    g.setColour(secondaryText()); g.setFont(juce::FontOptions(12.5f).withStyle("bold"));
    g.drawText("REPEAT PATTERN",140.f,20.f,260.f,20.f,juce::Justification::left);
    g.setColour(muted()); g.setFont(juce::FontOptions(9.5f));
    g.drawText("INTERACTIVE PATTERN EDITOR   /   POURIA MOTABEAN",140.f,39.f,400.f,16.f,juce::Justification::left);
    g.setColour(border().withAlpha(0.3f));
    g.drawLine(24.f,66.f,(float)getWidth()-24.f,66.f,1.0f);
}

void PDAudioProcessorEditor::drawBpmStatus(juce::Graphics& g, juce::Rectangle<float> box) const {
    drawPremiumPanel(g,box,9.f);
    float elapsed=p.uiElapsedSinceTrigger.load();
    bool recent = elapsed<0.22f;
    juce::Colour dotColour = recent ? sidePeak() : accentHighlight().withAlpha(0.55f);
    g.setColour(dotColour); g.fillEllipse(box.getX()+15.f,box.getCentreY()-4.5f,9.f,9.f);
    if(recent){
        g.setColour(dotColour.withAlpha(0.15f*(1.0f-elapsed/0.22f)));
        g.fillEllipse(box.getX()+15.f-4.f,box.getCentreY()-4.5f-4.f,17.f,17.f);
    }
    g.setColour(text()); g.setFont(juce::FontOptions(13.5f).withStyle("bold"));
    g.drawText(juce::String(p.uiBpm.load(),1)+" BPM",box.getX()+32.f,box.getY(),box.getWidth()-40.f,box.getHeight(),juce::Justification::centredLeft);
}

void PDAudioProcessorEditor::drawEditorPanel(juce::Graphics& g) const {
    drawPremiumPanel(g,editorPanelArea,10.f);

    bool custom=isCustomPresetActive();
    const auto& displayPattern = workingPattern;

    g.setColour(secondaryText()); g.setFont(juce::FontOptions(11.5f).withStyle("bold"));
    juce::String modeName = currentEditMode==EditMode::Volume ? "VOLUME" : "PAN";
    g.drawText(modeName,editorPanelArea.getX()+16.f,editorPanelArea.getY()+8.f,160.f,16.f,juce::Justification::left);
    g.setColour(muted()); g.setFont(juce::FontOptions(9.5f));
    juce::String meta = juce::String(displayPattern.count)+(displayPattern.count==1?" REPEAT":" REPEATS")+"  \u2022  1 BAR";
    if(!custom) meta += "  \u2022  POSITION LOCKED";
    g.drawText(meta,editorPanelArea.getX()+16.f,editorPanelArea.getY()+24.f,340.f,14.f,juce::Justification::left);
    if(custom && displayPattern.count==0){
        g.setColour(muted()); g.setFont(juce::FontOptions(9.5f));
        g.drawText("click in the graph to add a repeat",editorPanelArea.getRight()-260.f,editorPanelArea.getY()+24.f,244.f,14.f,juce::Justification::right);
    }

    drawGraph(g);
}

void PDAudioProcessorEditor::drawGraph(juce::Graphics& g) const {
    // Recessed inner surface
    g.setColour(recess()); g.fillRoundedRectangle(graphArea.expanded(1.0f),7.f);
    g.setColour(secondaryBg()); g.fillRoundedRectangle(graphArea,7.f);
    g.setColour(border().withAlpha(0.6f)); g.drawRoundedRectangle(graphArea,7.f,1.0f);

    // Reference gridlines - dB scale in Volume mode, L/C/R pan scale in Pan mode (spec section 5:
    // "visual labels should clearly communicate L100 / CENTER / R100").
    if(currentEditMode==EditMode::Volume){
        for(float v : {-12.f,-6.f,0.f,6.f,12.f}){
            float y=editValueToY(v);
            g.setColour(border().withAlpha(v==0.f?0.5f:0.18f));
            g.drawLine(graphArea.getX(),y,graphArea.getRight(),y,v==0.f?1.3f:1.0f);
            g.setColour(muted().withAlpha(0.8f)); g.setFont(juce::FontOptions(8.5f));
            juce::String lbl = (v==0.f?"0":(v>0?"+":"")+juce::String((int)v));
            g.drawText(lbl,graphArea.getX()-26.f,y-6.f,22.f,12.f,juce::Justification::right);
        }
    } else {
        for(float v : {-100.f,-50.f,0.f,50.f,100.f}){
            float y=editValueToY(v);
            g.setColour(border().withAlpha(v==0.f?0.5f:0.18f));
            g.drawLine(graphArea.getX(),y,graphArea.getRight(),y,v==0.f?1.3f:1.0f);
            g.setColour(muted().withAlpha(0.8f)); g.setFont(juce::FontOptions(8.5f));
            juce::String lbl = v==0.f?"C":(v<0.f?("L"+juce::String((int)-v)):("R"+juce::String((int)v)));
            g.drawText(lbl,graphArea.getX()-28.f,y-6.f,24.f,12.f,juce::Justification::right);
        }
    }

    bool custom=isCustomPresetActive();
    const auto& pat = workingPattern;

    // Live trigger position (read-only, from existing safe atomics) for a brief highlight pulse
    double bpm=p.uiBpm.load(); int numBeats=juce::jmax(1,p.uiTimeSigNumerator.load());
    double measureSec=(60.0/juce::jmax(1.0,bpm))*numBeats;
    float measureFrac = measureSec>0.0 ? (float)((double)p.uiElapsedSinceTrigger.load()/measureSec) : 999.f;

    for(int i=0;i<pat.count;++i){
        const auto& ev=pat.events[i];
        if(!ev.enabled) continue;
        auto handle=eventHandlePoint(ev);
        float baseline=graphArea.getCentreY();
        bool selected = (i==selectedEventIndex);
        bool hovered = (i==hoveredEventIndex) && !selected;
        bool activeNow = measureFrac>=ev.position && measureFrac<ev.position+0.05f;

        juce::Colour barColour = activeNow ? sidePeak() : (selected ? accentHighlight() : accent());
        if(activeNow){
            g.setColour(sidePeak().withAlpha(0.12f));
            g.fillRoundedRectangle(handle.x-4.f,juce::jmin(baseline,handle.y),8.f,std::abs(handle.y-baseline),3.f);
        }
        g.setColour(barColour);
        g.drawLine(handle.x,baseline,handle.x,handle.y,selected?2.4f:1.8f);

        float r = selected?7.f:(hovered?6.5f:5.5f);
        g.setColour(secondaryBg()); g.fillEllipse(handle.x-r,handle.y-r,r*2.f,r*2.f);
        g.setColour(selected?text():(hovered?accentHighlight():barColour));
        g.drawEllipse(handle.x-r,handle.y-r,r*2.f,r*2.f,selected?2.0f:1.4f);

        if(selected && dragMode!=DragMode::None){
            juce::String label;
            if(dragMode==DragMode::Position){
                label=juce::String((int)std::round(ev.position*100.f))+"%";
            } else if(currentEditMode==EditMode::Volume){
                label=juce::String(ev.volumeDb,1)+" dB";
            } else {
                float pv=ev.pan*100.f;
                label = std::abs(pv)<0.5f ? "C" : (pv<0.f?("L"+juce::String((int)std::round(-pv))):("R"+juce::String((int)std::round(pv))));
            }
            float boxW=64.f,boxH=20.f;
            float bx=juce::jlimit(graphArea.getX(),graphArea.getRight()-boxW,handle.x-boxW*0.5f);
            float by=handle.y-boxH-12.f;
            if(by<graphArea.getY()) by=handle.y+12.f;
            juce::Rectangle<float> box(bx,by,boxW,boxH);
            g.setColour(recess()); g.fillRoundedRectangle(box,4.f);
            g.setColour(border()); g.drawRoundedRectangle(box,4.f,1.0f);
            g.setColour(text()); g.setFont(juce::FontOptions(11.5f).withStyle("bold"));
            g.drawText(label,box,juce::Justification::centred);
        }
    }

    // Ruler
    int numBeatsForRuler=juce::jmax(1,p.uiTimeSigNumerator.load());
    auto rulerArea=juce::Rectangle<float>(graphArea.getX(),graphArea.getBottom()+6.f,graphArea.getWidth(),18.f);
    g.setColour(border().withAlpha(0.35f));
    const int subTicks=4;
    for(int i=0;i<=numBeatsForRuler*subTicks;++i){
        float t=(float)i/(float)(numBeatsForRuler*subTicks);
        float x=rulerArea.getX()+rulerArea.getWidth()*t;
        bool major=(i%subTicks)==0;
        g.setColour(border().withAlpha(major?0.4f:0.15f));
        g.drawLine(x,rulerArea.getY(),x,rulerArea.getY()+(major?7.f:4.f),1.0f);
    }
    g.setColour(muted()); g.setFont(juce::FontOptions(8.5f).withStyle("bold"));
    for(int b=0;b<=numBeatsForRuler;++b){
        float t=(float)b/(float)numBeatsForRuler;
        float x=rulerArea.getX()+rulerArea.getWidth()*t;
        juce::String label=(b==numBeatsForRuler)?"2.1":("1."+juce::String(b+1));
        g.drawText(label,x-20.f,rulerArea.getY()+8.f,40.f,10.f,
                   b==0?juce::Justification::left:(b==numBeatsForRuler?juce::Justification::right:juce::Justification::centred));
    }

    if(!custom){
        g.setColour(muted().withAlpha(0.65f)); g.setFont(juce::FontOptions(10.f));
        juce::String hint = juce::String("drag nodes to adjust ")+(currentEditMode==EditMode::Volume?"volume":"pan")+" - position is locked";
        g.drawText(hint,graphArea.getRight()-260.f,graphArea.getY()+6.f,250.f,16.f,juce::Justification::right);
    }
}

void PDAudioProcessorEditor::drawFooter(juce::Graphics& g, juce::Rectangle<float> area) const {
    g.setColour(border().withAlpha(0.25f));
    g.drawLine(area.getX(),area.getY(),area.getRight(),area.getY(),1.0f);
    g.setColour(muted()); g.setFont(juce::FontOptions(9.f));
    g.drawText("PD  \u2022  INTERACTIVE PATTERN EDITOR",area.getX(),area.getY()+4.f,400.f,area.getHeight()-4.f,juce::Justification::left);
    g.setColour(secondaryText()); g.setFont(juce::FontOptions(9.f).withStyle("bold"));
    juce::String grain="GRAIN "+juce::String((int)std::round(p.apvts.getRawParameterValue(PDAudioProcessor::pGrainMs)->load()))+" ms";
    g.drawText(grain,area.getRight()-160.f,area.getY()+4.f,160.f,area.getHeight()-4.f,juce::Justification::right);
}

void PDAudioProcessorEditor::paint(juce::Graphics& g){
    g.fillAll(bg());
    drawHeader(g);
    juce::Rectangle<float> bpmBox((float)getWidth()-300.f,20.f,150.f,36.f);
    drawBpmStatus(g,bpmBox);
    drawEditorPanel(g);
    drawFooter(g,{24.f,(float)getHeight()-26.f,(float)getWidth()-48.f,22.f});
}

void PDAudioProcessorEditor::resized(){
    const int w=getWidth(), h=getHeight();
    bypassBtn.setBounds(w-112,20,88,28);

    const int by=80;
    forwardBtn.setBounds(24,by,80,26);
    reverseBtn.setBounds(108,by,80,26);
    customBtn.setBounds(192,by,80,26);

    int mx=300;
    volumeModeBtn.setBounds(mx,by,68,26); mx+=72;
    panModeBtn.setBounds(mx,by,60,26); mx+=64;
    pitchModeBtn.setBounds(mx,by,60,26); mx+=64;
    formantModeBtn.setBounds(mx,by,76,26); mx+=80;
    reverseModeBtn.setBounds(mx,by,76,26);

    snapBtn.setBounds(w-24-64,by,64,26);

    editorPanelArea = {24.f,120.f,(float)w-48.f,(float)h-120.f-40.f};
    graphArea = editorPanelArea.reduced(40.f,44.f).withTrimmedBottom(20.f);
}
