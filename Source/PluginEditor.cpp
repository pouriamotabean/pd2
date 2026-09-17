#include "PluginEditor.h"

namespace {
void knob(juce::Slider& s){
    s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle(juce::Slider::NoTextBox,false,0,0);
    s.setColour(juce::Slider::rotarySliderFillColourId,juce::Colour(0xff78b7e8));
    s.setColour(juce::Slider::rotarySliderOutlineColourId,juce::Colour(0xff27313c));
    s.setColour(juce::Slider::thumbColourId,juce::Colour(0xffeef3f7));
}
void valueLabel(juce::Label& l){
    l.setJustificationType(juce::Justification::centred);
    l.setColour(juce::Label::textColourId,juce::Colour(0xffeef3f7));
    l.setFont(juce::FontOptions(12).withStyle("bold"));
}
}

PDAudioProcessorEditor::PDAudioProcessorEditor(PDAudioProcessor& proc):AudioProcessorEditor(&proc),p(proc){
    setSize(820,570);
    addAndMakeVisible(rateSection); addAndMakeVisible(curveSection); addAndMakeVisible(targetSection); addAndMakeVisible(shapeSection);

    addAndMakeVisible(bypassBtn); bypassBtn.setClickingTogglesState(true);
    bypassBtn.setColour(juce::TextButton::buttonColourId,juce::Colour(0xff121820));
    bypassBtn.setColour(juce::TextButton::buttonOnColourId,juce::Colour(0xff78b7e8));
    bypassBtn.setColour(juce::TextButton::textColourOffId,text()); bypassBtn.setColour(juce::TextButton::textColourOnId,juce::Colours::black);
    bypassBtn.onClick=[this]{setParam("bypass",bypassBtn.getToggleState()?1.f:0.f);};

    addAndMakeVisible(bpmBtn); addAndMakeVisible(hzBtn);
    bpmBtn.onClick=[this]{setParam("sync",1.f);refreshSyncControls();};
    hzBtn.onClick=[this]{setParam("sync",0.f);refreshSyncControls();};

    for(auto* box:{&startDivBox,&endDivBox}){addAndMakeVisible(*box);box->setColour(juce::ComboBox::backgroundColourId,juce::Colour(0xff111821));box->setColour(juce::ComboBox::outlineColourId,border());box->setColour(juce::ComboBox::textColourId,text());box->setColour(juce::ComboBox::arrowColourId,muted());for(int i=0;i<PDAudioProcessor::kNumDivisions;++i)box->addItem(PDAudioProcessor::kDivisionNames[i],i+1);}
    startDivBox.onChange=[this]{setParam("startDiv",(float)(startDivBox.getSelectedId()-1)/6.f);};
    endDivBox.onChange=[this]{setParam("endDiv",(float)(endDivBox.getSelectedId()-1)/6.f);};

    for(auto* s:{&startHzSlider,&endHzSlider,&decelTimeSlider,&depthSlider}){knob(*s);addAndMakeVisible(*s);}
    startHzSlider.setRange(.5,20,.01);endHzSlider.setRange(.5,20,.01);decelTimeSlider.setRange(.1,10,.01);depthSlider.setRange(0,100,.1);
    startHzSlider.onValueChange=[this]{setParam("startHz",(float)((startHzSlider.getValue()-.5)/19.5));startValue.setText(juce::String(startHzSlider.getValue(),2)+" Hz",juce::dontSendNotification);};
    endHzSlider.onValueChange=[this]{setParam("endHz",(float)((endHzSlider.getValue()-.5)/19.5));endValue.setText(juce::String(endHzSlider.getValue(),2)+" Hz",juce::dontSendNotification);};
    decelTimeSlider.onValueChange=[this]{setParam("decel",(float)((decelTimeSlider.getValue()-.1)/9.9));decelValue.setText(juce::String(decelTimeSlider.getValue(),2)+" s",juce::dontSendNotification);};
    depthSlider.onValueChange=[this]{setParam("depth",(float)depthSlider.getValue()/100.f);depthValue.setText(juce::String((int)depthSlider.getValue())+" %",juce::dontSendNotification);};
    for(auto* l:{&startValue,&endValue,&decelValue,&depthValue,&triggerValue,&inputValue}){valueLabel(*l);addAndMakeVisible(*l);}

    for(auto* b:{&expBtn,&linBtn,&logBtn})addAndMakeVisible(*b);
    expBtn.onClick=[this]{setParam("curve",0.f);refreshCurveButtons();};linBtn.onClick=[this]{setParam("curve",.5f);refreshCurveButtons();};logBtn.onClick=[this]{setParam("curve",1.f);refreshCurveButtons();};
    for(auto* b:{&ampBtn,&filterBtn,&pitchBtn})addAndMakeVisible(*b);
    ampBtn.onClick=[this]{setParam("target",0.f);refreshTargetButtons();};filterBtn.onClick=[this]{setParam("target",.5f);refreshTargetButtons();};pitchBtn.onClick=[this]{setParam("target",1.f);refreshTargetButtons();};
    for(auto* b:{&sineBtn,&triBtn,&sqBtn,&sawBtn})addAndMakeVisible(*b);
    sineBtn.onClick=[this]{setParam("shape",0.f);refreshShapeButtons();};triBtn.onClick=[this]{setParam("shape",1.f/3.f);refreshShapeButtons();};sqBtn.onClick=[this]{setParam("shape",2.f/3.f);refreshShapeButtons();};sawBtn.onClick=[this]{setParam("shape",1.f);refreshShapeButtons();};

    triggerValue.setJustificationType(juce::Justification::centredLeft); inputValue.setJustificationType(juce::Justification::centredRight);
    refreshFromProcessor(); startTimerHz(30);
}
PDAudioProcessorEditor::~PDAudioProcessorEditor(){stopTimer();}

void PDAudioProcessorEditor::setParam(const char* id,float n){if(auto* q=p.apvts.getParameter(id))q->setValueNotifyingHost(juce::jlimit(0.f,1.f,n));}
float PDAudioProcessorEditor::paramValue(const char* id)const{if(auto* q=p.apvts.getRawParameterValue(id))return q->load();return 0.f;}

void PDAudioProcessorEditor::refreshSyncControls(){bool sync=paramValue("sync")>.5f;bpmBtn.setToggleState(sync,juce::dontSendNotification);hzBtn.setToggleState(!sync,juce::dontSendNotification);startDivBox.setVisible(sync);endDivBox.setVisible(sync);startHzSlider.setVisible(!sync);endHzSlider.setVisible(!sync);startValue.setVisible(!sync);endValue.setVisible(!sync);}
void PDAudioProcessorEditor::refreshCurveButtons(){int v=(int)std::lround(paramValue("curve")*2.f);expBtn.setToggleState(v==0,juce::dontSendNotification);linBtn.setToggleState(v==1,juce::dontSendNotification);logBtn.setToggleState(v==2,juce::dontSendNotification);}
void PDAudioProcessorEditor::refreshTargetButtons(){int v=(int)std::lround(paramValue("target")*2.f);ampBtn.setToggleState(v==0,juce::dontSendNotification);filterBtn.setToggleState(v==1,juce::dontSendNotification);pitchBtn.setToggleState(v==2,juce::dontSendNotification);}
void PDAudioProcessorEditor::refreshShapeButtons(){int v=(int)std::lround(paramValue("shape")*3.f);sineBtn.setToggleState(v==0,juce::dontSendNotification);triBtn.setToggleState(v==1,juce::dontSendNotification);sqBtn.setToggleState(v==2,juce::dontSendNotification);sawBtn.setToggleState(v==3,juce::dontSendNotification);}
void PDAudioProcessorEditor::refreshFromProcessor(){
    bypassBtn.setToggleState(paramValue("bypass")>.5f,juce::dontSendNotification);
    int sd=(int)std::lround(paramValue("startDiv")*6.f),ed=(int)std::lround(paramValue("endDiv")*6.f);startDivBox.setSelectedId(sd+1,juce::dontSendNotification);endDivBox.setSelectedId(ed+1,juce::dontSendNotification);
    startHzSlider.setValue(.5+19.5*paramValue("startHz"),juce::dontSendNotification);endHzSlider.setValue(.5+19.5*paramValue("endHz"),juce::dontSendNotification);decelTimeSlider.setValue(.1+9.9*paramValue("decel"),juce::dontSendNotification);depthSlider.setValue(100*paramValue("depth"),juce::dontSendNotification);
    startValue.setText(juce::String(startHzSlider.getValue(),2)+" Hz",juce::dontSendNotification);endValue.setText(juce::String(endHzSlider.getValue(),2)+" Hz",juce::dontSendNotification);decelValue.setText(juce::String(decelTimeSlider.getValue(),2)+" s",juce::dontSendNotification);depthValue.setText(juce::String((int)depthSlider.getValue())+" %",juce::dontSendNotification);
    refreshSyncControls();refreshCurveButtons();refreshTargetButtons();refreshShapeButtons();
}

void PDAudioProcessorEditor::timerCallback(){refreshFromProcessor();const float e=p.uiElapsedSinceTrigger.load();triggerValue.setText(e<999.f?juce::String(e,2)+" s since trigger":"WAITING FOR TRANSIENT",juce::dontSendNotification);inputValue.setText(juce::String(p.uiInputLevelDb.load(),1)+" dB IN",juce::dontSendNotification);repaint();}

void PDAudioProcessorEditor::paint(juce::Graphics& g){
    g.fillAll(bg());
    g.setColour(text());g.setFont(juce::FontOptions(25).withStyle("bold"));g.drawText("PD",24,18,70,32,juce::Justification::left);
    g.setColour(muted());g.setFont(juce::FontOptions(11).withStyle("bold"));g.drawText("DECELERATING MODULATOR",88,24,260,20,juce::Justification::left);
    g.setColour(accent());g.setFont(juce::FontOptions(10).withStyle("bold"));g.drawText("TRANSIENT → FAST → SLOW → RETRIGGER",24,53,360,16,juce::Justification::left);

    auto graph=juce::Rectangle<float>(24,78,getWidth()-48,165);g.setColour(panel());g.fillRoundedRectangle(graph,12);g.setColour(border());g.drawRoundedRectangle(graph,.5f,12);
    g.setColour(muted());g.setFont(juce::FontOptions(10).withStyle("bold"));g.drawText("RATE OVER TIME",graph.getX()+14,graph.getY()+10,150,16,juce::Justification::left);
    drawRateGraph(g,graph.reduced(16).withTrimmedTop(30));
    bool recent=p.uiElapsedSinceTrigger.load()<1.5f;g.setColour(recent?accent():juce::Colour(0xff4b5661));g.fillEllipse(graph.getRight()-126,graph.getY()+12,8,8);
    g.setColour(muted());g.drawText("LIVE",graph.getRight()-112,graph.getY()+8,36,16,juce::Justification::left);
    g.setColour(muted());g.drawText("BPM",graph.getRight()-76,graph.getY()+8,30,16,juce::Justification::right);g.setColour(text());g.drawText(juce::String(p.currentBpm,1),graph.getRight()-42,graph.getY()+8,28,16,juce::Justification::right);
    g.setColour(muted());g.drawText("TRIGGERS "+juce::String(p.uiTriggerCount.load()),graph.getX()+14,graph.getBottom()-22,130,14,juce::Justification::left);
}

void PDAudioProcessorEditor::drawRateGraph(juce::Graphics& g,juce::Rectangle<float> r){
    g.setColour(juce::Colour(0xff202a34));for(int i=0;i<=4;++i){float x=r.getX()+r.getWidth()*i/4.f;g.drawVerticalLine((int)x,r.getY(),r.getBottom());}
    float startHz,endHz;if(paramValue("sync")>.5f){int si=(int)std::lround(paramValue("startDiv")*6.f),ei=(int)std::lround(paramValue("endDiv")*6.f);startHz=(float)(p.currentBpm/(60.0*PDAudioProcessor::kDivisionBeats[juce::jlimit(0,6,si)]));endHz=(float)(p.currentBpm/(60.0*PDAudioProcessor::kDivisionBeats[juce::jlimit(0,6,ei)]));}else{startHz=.5f+19.5f*paramValue("startHz");endHz=.5f+19.5f*paramValue("endHz");}
    auto c=(PDAudioProcessor::DecelCurve)juce::jlimit(0,2,(int)std::lround(paramValue("curve")*2.f));float hi=juce::jmax(startHz,endHz),lo=juce::jmin(startHz,endHz);juce::Path path,fill;const int N=120;
    for(int i=0;i<=N;++i){float t=(float)i/N,sh=PDAudioProcessor::shapeCurve(t,c),rate=startHz+(endHz-startHz)*sh,n=(hi>lo)?(rate-lo)/(hi-lo):.5f,x=r.getX()+r.getWidth()*t,y=r.getBottom()-r.getHeight()*n;if(i==0){path.startNewSubPath(x,y);fill.startNewSubPath(x,r.getBottom());fill.lineTo(x,y);}else{path.lineTo(x,y);fill.lineTo(x,y);}}
    fill.lineTo(r.getRight(),r.getBottom());fill.closeSubPath();g.setColour(accent().withAlpha(.10f));g.fillPath(fill);g.setColour(accent());g.strokePath(path,juce::PathStrokeType(2.2f));
    g.setColour(text());g.setFont(juce::FontOptions(10));g.drawText(juce::String(startHz,1)+" Hz",r.getX(),r.getY()+4,70,14,juce::Justification::left);g.drawText(juce::String(endHz,1)+" Hz",r.getX(),r.getBottom()-18,70,14,juce::Justification::left);
}

void PDAudioProcessorEditor::resized(){
    const int w=getWidth(), gap=14; bypassBtn.setBounds(w-112,18,88,28);bpmBtn.setBounds(w-190,18,36,28);hzBtn.setBounds(w-148,18,36,28);
    rateSection.setBounds(24,255,w-48,105);curveSection.setBounds(24,374,250,82);targetSection.setBounds(288,374,250,82);shapeSection.setBounds(552,374,w-576,82);
    startHzSlider.setBounds(42,278,62,62);endHzSlider.setBounds(176,278,62,62);decelTimeSlider.setBounds(310,278,62,62);depthSlider.setBounds(w-118,278,62,62);
    startDivBox.setBounds(34,294,100,28);endDivBox.setBounds(168,294,100,28);
    startValue.setBounds(32,337,112,16);endValue.setBounds(166,337,112,16);decelValue.setBounds(300,337,82,16);depthValue.setBounds(w-130,337,86,16);
    expBtn.setBounds(40,406,64,28);linBtn.setBounds(110,406,64,28);logBtn.setBounds(180,406,64,28);
    ampBtn.setBounds(304,406,104,28);filterBtn.setBounds(414,406,76,28);pitchBtn.setBounds(496,406,72,28);
    sineBtn.setBounds(568,406,58,28);triBtn.setBounds(632,406,72,28);sqBtn.setBounds(710,406,58,28);sawBtn.setBounds(774,406,36,28);
    triggerValue.setBounds(30,476,w/2-30,24);inputValue.setBounds(w/2,476,w-30,24);
}
