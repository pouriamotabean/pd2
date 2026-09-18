#include "PluginEditor.h"

// =====================================================================================
// PDButtonLookAndFeel - premium button depth (spec sections 11-13, 27, 83)
// Only touches drawing. Button behaviour (click/toggle) is 100% unchanged elsewhere.
// =====================================================================================
void PDButtonLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                                bool isHighlighted, bool isDown){
    auto bounds = button.getLocalBounds().toFloat();
    const bool active = button.getToggleState();
    const float radius = 6.0f;

    // Outer dark recess (spec #11) - the button appears to sit slightly INTO the panel
    g.setColour(juce::Colour(0xff080b0f));
    g.fillRoundedRectangle(bounds, radius);

    // Main face - shifts down ~1.5px when pressed (spec #12: 1-2px press shift)
    auto face = bounds.reduced(0.5f).withTrimmedBottom(isDown?0.5f:2.0f);
    if(isDown) face = face.translated(0.f, 1.5f);

    juce::Colour faceColour = active ? juce::Colour(0xff1d2633) : juce::Colour(0xff10161e);
    if(isHighlighted && !isDown) faceColour = faceColour.interpolatedWith(juce::Colour(0xff3a4148), 0.30f);
    g.setColour(faceColour);
    g.fillRoundedRectangle(face, radius);

    // Top highlight - reduced when pressed (spec #12)
    float highlightAlpha = isDown ? 0.08f : (active ? 0.30f : 0.18f);
    juce::Path topHighlight;
    topHighlight.addRoundedRectangle(face.getX(), face.getY(), face.getWidth(), juce::jmax(2.0f,face.getHeight()*0.45f),
                                      radius, radius, true, true, false, false);
    g.setColour(juce::Colour(0xff313942).withAlpha(highlightAlpha));
    g.fillPath(topHighlight);

    // Border - accent when active, subtle grid colour otherwise, brightens slightly on hover (spec #13)
    juce::Colour borderColour = active ? juce::Colour(0xff296095) : juce::Colour(0xff313942);
    if(isHighlighted && !active) borderColour = borderColour.brighter(0.2f);
    g.setColour(borderColour);
    g.drawRoundedRectangle(face, radius, active?1.4f:1.0f);
}

void PDButtonLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                          bool isHighlighted, bool isDown){
    const bool active = button.getToggleState();
    juce::Colour col = active ? juce::Colour(0xff69a1d0) : juce::Colour(0xff6f7a86);
    if(isHighlighted && !active) col = col.brighter(0.25f);
    g.setColour(col);
    g.setFont(juce::FontOptions(11.5f).withStyle("bold"));
    auto bounds = button.getLocalBounds();
    if(isDown) bounds = bounds.translated(0, 1);
    g.drawText(button.getButtonText(), bounds, juce::Justification::centred);
}

// =====================================================================================
// PDAudioProcessorEditor
// =====================================================================================
PDAudioProcessorEditor::PDAudioProcessorEditor(PDAudioProcessor& proc):AudioProcessorEditor(&proc),p(proc){
    setSize(1260,650);

    // All six buttons share the same premium LookAndFeel and the same behavioural pattern
    // (setClickingTogglesState + onClick calling setValueNotifyingHost on the real parameter) -
    // nothing about button BEHAVIOUR changed from before, only how they're drawn.
    for(auto* btn:{&bypassBtn,&forwardBtn,&reverseBtn,&curve1Btn,&curve2Btn,&curve3Btn}){
        addAndMakeVisible(*btn);
        btn->setClickingTogglesState(true);
        btn->setLookAndFeel(&pdLnf);
    }

    bypassBtn.onClick=[this]{
        if(auto* a=p.apvts.getParameter(PDAudioProcessor::pBypass))
            a->setValueNotifyingHost(bypassBtn.getToggleState()?1.f:0.f);
    };

    // FIX (requested): Forward/Reverse preset buttons - plain radio behaviour (clicking one selects
    // it and deselects the other), driven through the same pPattern choice parameter the processor
    // already reads in scheduleTapsForTrigger(), so host automation of this parameter also updates
    // these buttons correctly (see updatePatternButtonStates(), called every timer tick).
    forwardBtn.onClick=[this]{
        if(auto* a=p.apvts.getParameter(PDAudioProcessor::pPattern)) a->setValueNotifyingHost(0.f);
        updatePatternButtonStates();
    };
    reverseBtn.onClick=[this]{
        if(auto* a=p.apvts.getParameter(PDAudioProcessor::pPattern)) a->setValueNotifyingHost(1.f);
        updatePatternButtonStates();
    };
    updatePatternButtonStates();

    // FIX (graphics upgrade): real UI for the pVolumeCurve parameter, which already fully existed and
    // worked processor-side (getVolumeCurveSnapshot/setVolumeCurvePointY/defaultVolumeCurve all
    // support 3 independent curve slots) but had no selector control before now. 3-choice parameter,
    // same setValueNotifyingHost pattern as Forward/Reverse - normalised value = index/(numChoices-1).
    curve1Btn.onClick=[this]{ if(auto* a=p.apvts.getParameter(PDAudioProcessor::pVolumeCurve)) a->setValueNotifyingHost(0.f/2.f); updateCurveButtonStates(); };
    curve2Btn.onClick=[this]{ if(auto* a=p.apvts.getParameter(PDAudioProcessor::pVolumeCurve)) a->setValueNotifyingHost(1.f/2.f); updateCurveButtonStates(); };
    curve3Btn.onClick=[this]{ if(auto* a=p.apvts.getParameter(PDAudioProcessor::pVolumeCurve)) a->setValueNotifyingHost(2.f/2.f); updateCurveButtonStates(); };
    updateCurveButtonStates();

    startTimerHz(30);
}
PDAudioProcessorEditor::~PDAudioProcessorEditor(){
    stopTimer();
    for(auto* btn:{&bypassBtn,&forwardBtn,&reverseBtn,&curve1Btn,&curve2Btn,&curve3Btn}) btn->setLookAndFeel(nullptr);
}

void PDAudioProcessorEditor::updatePatternButtonStates(){
    // FIX (spec #57 - avoid UI state desync): read directly from the parameter every time, never
    // cache a separate bool - APVTS stays the single source of truth, so host automation is always
    // reflected correctly here.
    int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pPattern)->load();
    forwardBtn.setToggleState(idx==0,juce::dontSendNotification);
    reverseBtn.setToggleState(idx==1,juce::dontSendNotification);
}
void PDAudioProcessorEditor::updateCurveButtonStates(){
    int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
    curve1Btn.setToggleState(idx==0,juce::dontSendNotification);
    curve2Btn.setToggleState(idx==1,juce::dontSendNotification);
    curve3Btn.setToggleState(idx==2,juce::dontSendNotification);
}

float PDAudioProcessorEditor::currentMeasureFraction() const {
    // FIX (graphics upgrade, spec #61/#62): read-only computation from existing safe atomics - no new
    // processor state, no processor logic touched, paint() stays purely a reader.
    double bpm=p.uiBpm.load();
    int numBeats=juce::jmax(1,p.uiTimeSigNumerator.load());
    double measureSec=(60.0/juce::jmax(1.0,bpm))*numBeats;
    if(measureSec<=0.0) return 999.f;
    return (float)((double)p.uiElapsedSinceTrigger.load()/measureSec);
}

void PDAudioProcessorEditor::timerCallback(){
    updatePatternButtonStates();
    updateCurveButtonStates();
    repaint();
}

static void drawRuler(juce::Graphics& g,juce::Rectangle<float> r,int numBeats,juce::Colour tickCol,juce::Colour textCol){
    g.setColour(tickCol.withAlpha(0.4f));
    const int kSubTicksPerBeat=4;
    for(int i=0;i<=numBeats*kSubTicksPerBeat;++i){
        float t=(float)i/(float)(numBeats*kSubTicksPerBeat);
        float x=r.getX()+r.getWidth()*t;
        bool major=(i%kSubTicksPerBeat)==0;
        g.setColour(tickCol.withAlpha(major?0.4f:0.18f));
        g.drawLine(x,r.getY(),x,r.getY()+(major?10.f:6.f),major?1.4f:1.f);
    }
    g.setColour(textCol); g.setFont(juce::FontOptions(9.5f).withStyle("bold"));
    for(int b=0;b<=numBeats;++b){
        float t=(float)b/(float)numBeats;
        float x=r.getX()+r.getWidth()*t;
        juce::String label = (b==numBeats) ? "2.1" : ("1."+juce::String(b+1));
        g.drawText(label,x-24.f,r.getY()+14.f,48.f,14.f, b==0?juce::Justification::left:(b==numBeats?juce::Justification::right:juce::Justification::centred));
    }
}

juce::Point<float> PDAudioProcessorEditor::volumePointToScreen(const PDAudioProcessor::VolumeCurve& c,int index) const {
    const auto& pt=c.points[index];
    float x=volumePlotArea.getX()+volumePlotArea.getWidth()*pt.x;
    // y range mapped: gain 0 -> bottom, gain 1.3 -> top (a little headroom above unity for "boosted" points)
    float norm=juce::jlimit(0.f,1.f,pt.y/1.3f);
    float y=volumePlotArea.getBottom()-volumePlotArea.getHeight()*norm;
    return {x,y};
}
int PDAudioProcessorEditor::hitTestVolumePoint(juce::Point<float> pos) const {
    // FIX (spec #37 - hit test bounds): only hit-test inside volumePlotArea, so mouse interaction
    // elsewhere (header, buttons, pattern panel, footer) can never be mistaken for a curve drag.
    if(!volumePlotArea.expanded(12.f).contains(pos)) return -1;
    int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
    const auto c=p.getVolumeCurveSnapshot(idx);
    for(int i=0;i<c.count;++i){
        auto sp=volumePointToScreen(c,i);
        if(sp.getDistanceFrom(pos)<12.f) return i;
    }
    return -1;
}
void PDAudioProcessorEditor::mouseDown(const juce::MouseEvent& e){
    draggedPointIndex=hitTestVolumePoint(e.position);
}
void PDAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e){
    if(draggedPointIndex<0) return;
    int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
    float norm=juce::jlimit(0.f,1.f,(volumePlotArea.getBottom()-e.position.y)/volumePlotArea.getHeight());
    p.setVolumeCurvePointY(idx,draggedPointIndex,juce::jlimit(0.f,1.3f,norm*1.3f));
    repaint();
}
void PDAudioProcessorEditor::mouseUp(const juce::MouseEvent&){
    // FIX (spec #38 - mouse state safety): always reset drag state on mouseUp, including if the
    // mouse was dragged outside the graph area before release - draggedPointIndex is set to -1
    // unconditionally here regardless of where the cursor ended up.
    draggedPointIndex=-1;
}
void PDAudioProcessorEditor::mouseMove(const juce::MouseEvent& e){
    int hit=hitTestVolumePoint(e.position);
    if(hit!=hoveredPointIndex){ hoveredPointIndex=hit; repaint(); }
}
void PDAudioProcessorEditor::mouseExit(const juce::MouseEvent&){
    if(hoveredPointIndex!=-1){ hoveredPointIndex=-1; repaint(); }
}
void PDAudioProcessorEditor::mouseDoubleClick(const juce::MouseEvent& e){
    // FIX (requested): double-clicking a volume-curve node resets JUST that node back to flat
    // (gain 1.0) - a quick way to undo one point's edit without resetting the whole curve.
    int hit=hitTestVolumePoint(e.position);
    if(hit>=0){
        int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
        p.setVolumeCurvePointY(idx,hit,1.0f);
        // FIX (spec #36): brief visual flash on the reset point, purely cosmetic editor-local state.
        flashedPointIndex=hit; flashUntilMs=juce::Time::getMillisecondCounter()+100;
        repaint();
    }
    // FIX (requested #1): the double-click pattern-switcher stays removed - there's a real, visible
    // Forward/Reverse selector now, so no gesture is required or hidden here (spec #54).
}

// ---- Premium drawing helpers (spec #69, #42-45) ----------------------------------------------
void PDAudioProcessorEditor::drawPremiumPanel(juce::Graphics& g, juce::Rectangle<float> bounds, float radius) const {
    // 4-layer depth system (spec #42): recess edge -> panel fill -> border -> subtle top inner highlight
    g.setColour(recess()); g.fillRoundedRectangle(bounds.expanded(1.0f), radius);
    g.setColour(panel()); g.fillRoundedRectangle(bounds, radius);
    juce::Path topHighlight;
    topHighlight.addRoundedRectangle(bounds.getX(),bounds.getY(),bounds.getWidth(),bounds.getHeight()*0.5f,
                                      radius,radius,true,true,false,false);
    g.setColour(raised().withAlpha(0.18f)); g.fillPath(topHighlight);
    g.setColour(border()); g.drawRoundedRectangle(bounds,radius,1.0f);
}
void PDAudioProcessorEditor::drawRecessedGraph(juce::Graphics& g, juce::Rectangle<float> bounds, float radius) const {
    g.setColour(recess()); g.fillRoundedRectangle(bounds.expanded(1.0f),radius);
    g.setColour(secondaryBg()); g.fillRoundedRectangle(bounds,radius);
    g.setColour(border().withAlpha(0.6f)); g.drawRoundedRectangle(bounds,radius,1.0f);
}

void PDAudioProcessorEditor::drawHeader(juce::Graphics& g, juce::Rectangle<float>) const {
    g.setColour(text()); g.setFont(juce::FontOptions(31.f).withStyle("bold"));
    g.drawText("PD",28.f,18.f,90.f,38.f,juce::Justification::left);

    // Thin vertical accent line beside PD (spec #6) - deliberately understated, not dominant
    g.setColour(accent().withAlpha(0.7f));
    g.fillRect(122.f,24.f,1.6f,32.f);

    g.setColour(secondaryText()); g.setFont(juce::FontOptions(12.5f).withStyle("bold"));
    g.drawText("REPEAT PATTERN",140.f,20.f,260.f,20.f,juce::Justification::left);
    g.setColour(muted()); g.setFont(juce::FontOptions(9.5f));
    g.drawText("RHYTHMIC GRAIN ENGINE",140.f,39.f,260.f,16.f,juce::Justification::left);

    // Very faint separator under the whole header (spec #7)
    g.setColour(border().withAlpha(0.3f));
    g.drawLine(24.f,66.f,(float)getWidth()-24.f,66.f,1.0f);
}

void PDAudioProcessorEditor::drawBpmStatus(juce::Graphics& g, juce::Rectangle<float> box) const {
    drawPremiumPanel(g,box,9.f);
    // FIX (spec #9): micro-animation window derived from uiElapsedSinceTrigger - no new timer, uses
    // the existing 30Hz UI timer already running for everything else.
    float elapsed=p.uiElapsedSinceTrigger.load();
    bool recent = elapsed<0.22f; // spec suggests 150-250ms pulse window
    juce::Colour dotColour = recent ? sidePeak() : accentHighlight().withAlpha(0.55f);
    g.setColour(dotColour); g.fillEllipse(box.getX()+15.f,box.getCentreY()-4.5f,9.f,9.f);
    if(recent){
        g.setColour(dotColour.withAlpha(0.15f*(1.0f-elapsed/0.22f)));
        g.fillEllipse(box.getX()+15.f-4.f,box.getCentreY()-4.5f-4.f,17.f,17.f);
    }
    g.setColour(text()); g.setFont(juce::FontOptions(13.5f).withStyle("bold"));
    g.drawText(juce::String(p.uiBpm.load(),1)+" BPM",box.getX()+32.f,box.getY(),box.getWidth()-40.f,box.getHeight(),juce::Justification::centredLeft);
}

void PDAudioProcessorEditor::drawPatternPanel(juce::Graphics& g) const {
    drawPremiumPanel(g,patternArea,10.f);
    g.setColour(secondaryText()); g.setFont(juce::FontOptions(11.5f).withStyle("bold"));
    g.drawText("PATTERN",patternArea.getX()+16.f,patternArea.getY()+10.f,160.f,16.f,juce::Justification::left);

    int patIdx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pPattern)->load();
    const auto& pat=PDAudioProcessor::getPattern(patIdx);
    int numBeats=juce::jmax(1,p.uiTimeSigNumerator.load());

    // Spec #17: small meta line under the title
    g.setColour(muted()); g.setFont(juce::FontOptions(9.5f));
    juce::String meta = juce::String(pat.count)+" TRIGGERS  \u2022  1 BAR";
    g.drawText(meta,patternArea.getX()+16.f,patternArea.getY()+27.f,240.f,14.f,juce::Justification::left);

    drawRecessedGraph(g,patternGraphArea,7.f);
    auto tickArea=patternGraphArea.reduced(16.f,10.f);
    drawPatternGraph(g,tickArea,pat);
    drawRuler(g,{tickArea.getX(),patternGraphArea.getBottom()+8.f,tickArea.getWidth(),22.f},numBeats,border(),muted());
}

void PDAudioProcessorEditor::drawPatternGraph(juce::Graphics& g, juce::Rectangle<float> tickArea, const PDAudioProcessor::Pattern& pat) const {
    g.setColour(border().withAlpha(0.5f));
    g.drawLine(tickArea.getX(),tickArea.getBottom(),tickArea.getRight(),tickArea.getBottom(),1.0f);

    if(pat.count==0){
        // FIX (spec #22): calm, non-error-looking empty state
        g.setColour(muted()); g.setFont(juce::FontOptions(11.5f));
        g.drawText("NO PATTERN DATA",tickArea,juce::Justification::centred);
        return;
    }

    const float measureFrac=currentMeasureFraction();
    const bool notePlaying = measureFrac>=0.f && measureFrac<1.05f; // roughly "within this cycle"
    for(int i=0;i<pat.count;++i){
        float x=tickArea.getX()+tickArea.getWidth()*pat.positions[i];
        // FIX (spec #18/#20/#61/#62): highlight the tap that playback has JUST passed, computed
        // read-only from live atomics - never a guessed/fabricated trigger index. Window kept small
        // (~6% of the measure) so only one (or zero) taps are ever lit at a time.
        bool active = notePlaying && measureFrac>=pat.positions[i] && measureFrac<pat.positions[i]+0.06f;
        if(active){
            g.setColour(sidePeak().withAlpha(0.12f));
            g.fillRoundedRectangle(x-4.f,tickArea.getY(),8.f,tickArea.getHeight(),3.f);
        }
        g.setColour(active?sidePeak():accent());
        g.drawLine(x,tickArea.getY(),x,tickArea.getBottom(),active?2.0f:1.6f);
    }
}

void PDAudioProcessorEditor::drawVolumePanel(juce::Graphics& g) const {
    drawPremiumPanel(g,volumeArea,10.f);
    g.setColour(secondaryText()); g.setFont(juce::FontOptions(11.5f).withStyle("bold"));
    g.drawText("VOLUME CURVE",volumeArea.getX()+16.f,volumeArea.getY()+10.f,200.f,16.f,juce::Justification::left);

    int curveIdx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
    const auto curve=p.getVolumeCurveSnapshot(curveIdx);
    g.setColour(muted()); g.setFont(juce::FontOptions(9.5f));
    g.drawText(juce::String(curve.count)+" POINTS  \u2022  drag to reshape, double-click to reset",
                volumeArea.getX()+16.f,volumeArea.getY()+27.f,340.f,14.f,juce::Justification::left);

    drawRecessedGraph(g,volumePlotArea.expanded(16.f,10.f),7.f);

    int numBeats=juce::jmax(1,p.uiTimeSigNumerator.load());

    // Reference gridlines (spec #31)
    for(float gval:{0.5f,1.0f}){
        float y=volumePlotArea.getBottom()-volumePlotArea.getHeight()*(gval/1.3f);
        g.setColour(border().withAlpha(gval>=1.0f?0.4f:0.18f));
        g.drawLine(volumePlotArea.getX(),y,volumePlotArea.getRight(),y,1.0f);
    }

    juce::Path curvePath;
    const int N=200;
    for(int i=0;i<=N;++i){
        float t=(float)i/N;
        float gval=PDAudioProcessor::evalVolumeCurve(curve,t);
        float x=volumePlotArea.getX()+volumePlotArea.getWidth()*t;
        float y=volumePlotArea.getBottom()-volumePlotArea.getHeight()*juce::jlimit(0.f,1.f,gval/1.3f);
        if(i==0) curvePath.startNewSubPath(x,y); else curvePath.lineTo(x,y);
    }

    // Subtle area fill under the curve (spec #30) - visual depth only, no DSP relevance whatsoever
    {
        juce::Path fillPath(curvePath);
        fillPath.lineTo(volumePlotArea.getRight(),volumePlotArea.getBottom());
        fillPath.lineTo(volumePlotArea.getX(),volumePlotArea.getBottom());
        fillPath.closeSubPath();
        g.setGradientFill(juce::ColourGradient(accent().withAlpha(0.16f),0,volumePlotArea.getY(),
                                                accent().withAlpha(0.0f),0,volumePlotArea.getBottom(),false));
        g.fillPath(fillPath);
    }
    // Very subtle glow under the curve line (spec #29) - drawn as a soft wide stroke behind the crisp one
    g.setColour(accent().withAlpha(0.08f));
    g.strokePath(curvePath,juce::PathStrokeType(6.0f));
    g.setColour(accentHighlight());
    g.strokePath(curvePath,juce::PathStrokeType(2.2f));

    juce::uint32 now=juce::Time::getMillisecondCounter();
    for(int i=0;i<curve.count;++i){
        auto sp=volumePointToScreen(curve,i);
        bool dragging=(draggedPointIndex==i);
        bool hovered=(hoveredPointIndex==i) && !dragging;
        bool flashing=(flashedPointIndex==i) && now<flashUntilMs;

        if(hovered||dragging||flashing){
            g.setColour(accent().withAlpha(0.13f));
            g.fillEllipse(sp.x-11.f,sp.y-11.f,22.f,22.f);
        }
        float outerR = dragging?8.f:(hovered?7.5f:7.f);
        g.setColour(secondaryBg()); g.fillEllipse(sp.x-outerR,sp.y-outerR,outerR*2.f,outerR*2.f);
        g.setColour(dragging?text():(hovered?accentHighlight():border()));
        g.drawEllipse(sp.x-outerR,sp.y-outerR,outerR*2.f,outerR*2.f,dragging?2.2f:1.6f);
        g.setColour(accentHighlight());
        g.fillEllipse(sp.x-3.f,sp.y-3.f,6.f,6.f);

        if(dragging){
            // Live floating value label (spec #35) - flips above the point if too close to the panel top
            juce::String label="GAIN";
            juce::String value=juce::String(curve.points[i].y,2)+" \u00d7";
            float boxW=76.f,boxH=34.f;
            float bx=juce::jlimit(volumePlotArea.getX(),volumePlotArea.getRight()-boxW,sp.x-boxW*0.5f);
            float by=sp.y-boxH-14.f;
            if(by<volumePlotArea.getY()) by=sp.y+14.f; // flip below if it would go above the panel
            juce::Rectangle<float> box(bx,by,boxW,boxH);
            g.setColour(recess()); g.fillRoundedRectangle(box,5.f);
            g.setColour(border()); g.drawRoundedRectangle(box,5.f,1.0f);
            g.setColour(muted()); g.setFont(juce::FontOptions(8.5f).withStyle("bold"));
            g.drawText(label,box.getX()+7.f,box.getY()+4.f,box.getWidth()-14.f,12.f,juce::Justification::left);
            g.setColour(text()); g.setFont(juce::FontOptions(13.f).withStyle("bold"));
            g.drawText(value,box.getX()+7.f,box.getY()+16.f,box.getWidth()-14.f,16.f,juce::Justification::left);
        }
    }
    drawRuler(g,{volumePlotArea.getX(),volumePlotArea.getBottom()+18.f,volumePlotArea.getWidth(),22.f},numBeats,border(),muted());
}

void PDAudioProcessorEditor::drawFooter(juce::Graphics& g, juce::Rectangle<float> area) const {
    g.setColour(border().withAlpha(0.25f));
    g.drawLine(area.getX(),area.getY(),area.getRight(),area.getY(),1.0f);
    g.setColour(muted()); g.setFont(juce::FontOptions(9.f));
    g.drawText("PD  \u2022  RHYTHMIC GRAIN ENGINE",area.getX(),area.getY()+4.f,400.f,area.getHeight()-4.f,juce::Justification::left);
    g.setColour(secondaryText()); g.setFont(juce::FontOptions(9.f).withStyle("bold"));
    juce::String grain="GRAIN "+juce::String((int)std::round(p.apvts.getRawParameterValue(PDAudioProcessor::pGrainMs)->load()))+" ms";
    g.drawText(grain,area.getRight()-160.f,area.getY()+4.f,160.f,area.getHeight()-4.f,juce::Justification::right);
}

void PDAudioProcessorEditor::paint(juce::Graphics& g){
    // FIX (spec #81 - paint must be read-only): this function only ever calls p.<atomic>.load(),
    // p.getVolumeCurveSnapshot() (a copy) and PDAudioProcessor::getPattern()/evalVolumeCurve() (pure,
    // static, read-only). Nothing here mutates a parameter, curve, or pattern.
    g.fillAll(bg());
    auto a=getLocalBounds().toFloat();

    drawHeader(g,a);

    juce::Rectangle<float> bpmBox(a.getRight()-300.f,20.f,150.f,36.f);
    drawBpmStatus(g,bpmBox);

    drawPatternPanel(g);
    drawVolumePanel(g);

    drawFooter(g,{24.f,(float)getHeight()-26.f,(float)getWidth()-48.f,22.f});
}

void PDAudioProcessorEditor::resized(){
    const int w=getWidth();
    bypassBtn.setBounds(w-112,66,88,28);

    patternArea={24.f,84.f,(float)w-48.f,250.f};
    volumeArea={24.f,352.f,(float)w-48.f,250.f};

    patternGraphArea = patternArea.withTrimmedTop(48.f).withTrimmedBottom(38.f).reduced(16.f,0.f);
    volumePlotArea = volumeArea.withTrimmedTop(50.f).withTrimmedBottom(46.f).reduced(36.f,0.f);

    forwardBtn.setBounds((int)patternArea.getRight()-192,(int)patternArea.getY()+10,90,24);
    reverseBtn.setBounds((int)patternArea.getRight()-98,(int)patternArea.getY()+10,90,24);

    const int curveBtnW=78;
    curve1Btn.setBounds((int)volumeArea.getRight()-(curveBtnW*3+8),(int)volumeArea.getY()+10,curveBtnW,24);
    curve2Btn.setBounds((int)volumeArea.getRight()-(curveBtnW*2+4),(int)volumeArea.getY()+10,curveBtnW,24);
    curve3Btn.setBounds((int)volumeArea.getRight()-curveBtnW,(int)volumeArea.getY()+10,curveBtnW,24);
}
