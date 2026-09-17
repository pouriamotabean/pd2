#include "PluginEditor.h"

PDAudioProcessorEditor::PDAudioProcessorEditor(PDAudioProcessor& proc):AudioProcessorEditor(&proc),p(proc){
    setSize(1260,650);
    addAndMakeVisible(bypassBtn);
    bypassBtn.setClickingTogglesState(true);
    bypassBtn.setColour(juce::TextButton::buttonColourId,juce::Colour(0xff121820));
    bypassBtn.setColour(juce::TextButton::buttonOnColourId,accent());
    bypassBtn.setColour(juce::TextButton::textColourOffId,text());
    bypassBtn.setColour(juce::TextButton::textColourOnId,juce::Colours::black);
    bypassBtn.onClick=[this]{
        if(auto* a=p.apvts.getParameter(PDAudioProcessor::pBypass))
            a->setValueNotifyingHost(bypassBtn.getToggleState()?1.f:0.f);
    };
    startTimerHz(30);
}
PDAudioProcessorEditor::~PDAudioProcessorEditor(){ stopTimer(); }

void PDAudioProcessorEditor::timerCallback(){ repaint(); }

static void drawRuler(juce::Graphics& g,juce::Rectangle<float> r,int numBeats,juce::Colour tickCol,juce::Colour textCol){
    g.setColour(tickCol);
    const int kSubTicksPerBeat=4;
    for(int i=0;i<=numBeats*kSubTicksPerBeat;++i){
        float t=(float)i/(float)(numBeats*kSubTicksPerBeat);
        float x=r.getX()+r.getWidth()*t;
        bool major=(i%kSubTicksPerBeat)==0;
        g.drawLine(x,r.getY(),x,r.getY()+(major?10.f:6.f),major?1.4f:1.f);
    }
    g.setColour(textCol); g.setFont(juce::FontOptions(11.f).withStyle("bold"));
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
    int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
    const auto& c=p.volumeCurves[juce::jlimit(0,2,idx)];
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
    auto& c=p.volumeCurves[juce::jlimit(0,2,idx)];
    float norm=juce::jlimit(0.f,1.f,(volumePlotArea.getBottom()-e.position.y)/volumePlotArea.getHeight());
    c.points[draggedPointIndex].y=juce::jlimit(0.f,1.3f,norm*1.3f);
    repaint();
}
void PDAudioProcessorEditor::mouseUp(const juce::MouseEvent&){ draggedPointIndex=-1; }
void PDAudioProcessorEditor::mouseDoubleClick(const juce::MouseEvent& e){
    // FIX (requested): double-clicking a volume-curve node resets JUST that node back to flat
    // (gain 1.0) - a quick way to undo one point's edit without resetting the whole curve.
    int hit=hitTestVolumePoint(e.position);
    if(hit>=0){
        int idx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
        p.volumeCurves[juce::jlimit(0,2,idx)].points[hit].y=1.0f;
        repaint();
        return;
    }
    // Temporary A/B-testing control (until the real preset panel is wired in): double-click the
    // pattern label in the schematic header to cycle between the built patterns.
    if(patternLabelHitBox.contains(e.position)){
        if(auto* a=p.apvts.getParameter(PDAudioProcessor::pPattern)){
            int cur=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pPattern)->load();
            int next=(cur+1)%3;
            a->setValueNotifyingHost((float)next/2.f);
        }
        repaint();
    }
}

void PDAudioProcessorEditor::paint(juce::Graphics& g){
    g.fillAll(bg());
    auto a=getLocalBounds().toFloat();

    // ---- Header ----
    g.setColour(text()); g.setFont(juce::FontOptions(30.f).withStyle("bold"));
    g.drawText("PD",28,20,90,38,juce::Justification::left);
    g.setColour(border()); g.drawLine(128,26,128,52,1.f);
    g.setColour(muted()); g.setFont(juce::FontOptions(13.f).withStyle("bold"));
    g.drawText("REPEAT PATTERN",144,26,260,26,juce::Justification::left);

    double bpm=p.uiBpm.load();
    juce::Rectangle<float> bpmBox(a.getRight()-300.f,20.f,150.f,36.f);
    g.setColour(panel()); g.fillRoundedRectangle(bpmBox,8.f);
    g.setColour(border()); g.drawRoundedRectangle(bpmBox,8.f,1.f);
    bool recent=p.uiElapsedSinceTrigger.load()<1.2f;
    g.setColour(recent?accent():juce::Colour(0xff4b5661)); g.fillEllipse(bpmBox.getX()+14.f,bpmBox.getCentreY()-5.f,10.f,10.f);
    g.setColour(text()); g.setFont(juce::FontOptions(14.f).withStyle("bold"));
    g.drawText(juce::String(bpm,1)+" BPM",bpmBox.getX()+32.f,bpmBox.getY(),bpmBox.getWidth()-40.f,bpmBox.getHeight(),juce::Justification::centredLeft);

    // ---- Pattern schematic ----
    g.setColour(panel()); g.fillRoundedRectangle(patternArea,12.f);
    g.setColour(border()); g.drawRoundedRectangle(patternArea,12.f,1.f);
    int patIdxForLabel=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pPattern)->load();
    juce::String patLabel="PATTERN "+juce::String(patIdxForLabel+1);
    patternLabelHitBox={patternArea.getX()+16.f,patternArea.getY()+10.f,120.f,16.f};
    g.setColour(muted()); g.setFont(juce::FontOptions(11.f).withStyle("bold"));
    g.drawText(patLabel,patternLabelHitBox,juce::Justification::left);
    g.setColour(muted().withAlpha(0.6f)); g.setFont(juce::FontOptions(9.5f));
    g.drawText("(double-click to A/B compare)",patternArea.getX()+120.f,patternArea.getY()+11.f,220.f,14.f,juce::Justification::left);

    int patIdx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pPattern)->load();
    const auto& pat=PDAudioProcessor::getPattern(patIdx);
    auto tickArea=patternArea.reduced(20.f,0.f).withTrimmedTop(40.f).withTrimmedBottom(46.f);
    g.setColour(border()); g.drawLine(tickArea.getX(),tickArea.getBottom(),tickArea.getRight(),tickArea.getBottom(),1.f);
    g.setColour(accent());
    for(int i=0;i<pat.count;++i){
        float x=tickArea.getX()+tickArea.getWidth()*pat.positions[i];
        g.drawLine(x,tickArea.getY(),x,tickArea.getBottom(),2.f);
    }
    if(pat.count==0){
        g.setColour(muted()); g.setFont(juce::FontOptions(12.f));
        g.drawText("(empty - not built yet)",tickArea,juce::Justification::centred);
    }
    int numBeats=juce::jmax(1,p.uiTimeSigNumerator.load());
    drawRuler(g,{tickArea.getX(),tickArea.getBottom()+10.f,tickArea.getWidth(),24.f},numBeats,juce::Colour(0xff3a4552),muted());

    // ---- Volume curve ----
    g.setColour(panel()); g.fillRoundedRectangle(volumeArea,12.f);
    g.setColour(border()); g.drawRoundedRectangle(volumeArea,12.f,1.f);
    g.setColour(muted()); g.setFont(juce::FontOptions(11.f).withStyle("bold"));
    g.drawText("VOLUME",volumeArea.getX()+16.f,volumeArea.getY()+10.f,200.f,16.f,juce::Justification::left);
    g.setColour(muted().withAlpha(0.7f)); g.setFont(juce::FontOptions(9.5f));
    g.drawText("drag the dots to reshape",volumeArea.getRight()-170.f,volumeArea.getY()+12.f,158.f,14.f,juce::Justification::right);

    int curveIdx=(int)p.apvts.getRawParameterValue(PDAudioProcessor::pVolumeCurve)->load();
    auto& curve=p.volumeCurves[juce::jlimit(0,2,curveIdx)];
    // faint reference gridlines at gain 0.5 and 1.0
    g.setColour(juce::Colour(0xff232d38));
    for(float gval:{0.5f,1.0f}){
        float y=volumePlotArea.getBottom()-volumePlotArea.getHeight()*(gval/1.3f);
        g.drawLine(volumePlotArea.getX(),y,volumePlotArea.getRight(),y,1.f);
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
    g.setColour(accent()); g.strokePath(curvePath,juce::PathStrokeType(2.2f));
    for(int i=0;i<curve.count;++i){
        auto sp=volumePointToScreen(curve,i);
        bool dragging=(draggedPointIndex==i);
        g.setColour(bg()); g.fillEllipse(sp.x-7.f,sp.y-7.f,14.f,14.f);
        g.setColour(dragging?juce::Colours::white:accent()); g.drawEllipse(sp.x-7.f,sp.y-7.f,14.f,14.f,2.f);
        if(dragging){ g.setColour(accent()); g.fillEllipse(sp.x-3.5f,sp.y-3.5f,7.f,7.f); }
    }
    drawRuler(g,{volumePlotArea.getX(),volumePlotArea.getBottom()+10.f,volumePlotArea.getWidth(),24.f},numBeats,juce::Colour(0xff3a4552),muted());
}

void PDAudioProcessorEditor::resized(){
    const int w=getWidth();
    bypassBtn.setBounds(w-112,66,88,28);
    patternArea={24.f,84.f,(float)w-48.f,250.f};
    volumeArea={24.f,352.f,(float)w-48.f,250.f};
    volumePlotArea = volumeArea.withTrimmedTop(38.f).withTrimmedBottom(40.f).reduced(20.f,0.f);
}
