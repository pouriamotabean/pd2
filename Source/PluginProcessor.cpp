#include "PluginProcessor.h"
#include "PluginEditor.h"

PDAudioProcessor::PDAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

bool PDAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

void PDAudioProcessor::prepareToPlay(double sampleRate, int) {
    sr=sampleRate;
    envFast=0.f; envSlow=0.f;
    timeSinceLastTrigger=1.0; phase=0.0; elapsed=999.0;
    filterStateL=0.f; filterStateR=0.f;
    pitchDelayL.fill(0.f); pitchDelayR.fill(0.f); pitchWriteIdx=0;
}

// The deceleration envelope's shape: 0->1 progress in, 0->1 "how far toward the end rate" out.
// Exponential = fast change early, levelling off (the classic decay-curve look from the reference
// mockup). Linear = constant rate of change. Logarithmic = slow early, fast late (the opposite feel).
float PDAudioProcessor::shapeCurve(float t, DecelCurve c) {
    t = juce::jlimit(0.f,1.f,t);
    switch (c) {
        case DecelCurve::Exponential: { constexpr float k=5.f; return (1.f-std::exp(-k*t))/(1.f-std::exp(-k)); }
        case DecelCurve::Logarithmic: return t*t;
        case DecelCurve::Linear: default: return t;
    }
}

float PDAudioProcessor::lfoWave(float phase01, Shape s) {
    switch (s) {
        case Shape::Triangle: return 2.f*std::abs(2.f*(phase01-std::floor(phase01+0.5f)))-1.f;
        case Shape::Square:   return phase01<0.5f? 1.f : -1.f;
        case Shape::Saw:      return 2.f*phase01-1.f;
        case Shape::Sine: default: return std::sin(juce::MathConstants<float>::twoPi*phase01);
    }
}

float PDAudioProcessor::currentRateFor(double elapsedSec) const {
    float startHz, endHz;
    if (bpmSync.load()) {
        int si=juce::jlimit(0,kNumDivisions-1,startDivIndex.load());
        int ei=juce::jlimit(0,kNumDivisions-1,endDivIndex.load());
        startHz=(float)(currentBpm/(60.0*kDivisionBeats[si]));
        endHz  =(float)(currentBpm/(60.0*kDivisionBeats[ei]));
    } else {
        startHz=startRateHz.load(); endHz=endRateHz.load();
    }
    float decelTime=juce::jmax(0.05f,decelTimeSec.load());
    float t=(float)(elapsedSec/decelTime);
    float shaped=shapeCurve(t,decelCurve.load());
    return startHz + (endHz-startHz)*shaped;
}

void PDAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int numCh=buffer.getNumChannels(), numSamples=buffer.getNumSamples();

    if (auto* ph=getPlayHead()) {
        if (auto pos=ph->getPosition(); pos.hasValue())
            if (auto bpmOpt=pos->getBpm(); bpmOpt.hasValue())
                currentBpm=juce::jmax(20.0,*bpmOpt);
    }

    if (bypassed.load()) return;

    auto* L=buffer.getWritePointer(0);
    auto* R=numCh>1? buffer.getWritePointer(1) : nullptr;
    const Target tgt=target.load();
    const Shape shp=shape.load();
    const float dep=juce::jlimit(0.f,1.f,depth.load());

    for (int i=0;i<numSamples;++i) {
        float in=L[i]; if (R) in=0.5f*(L[i]+R[i]);
        float rectified=std::abs(in);
        // Fast/slow envelope-follower onset detector: a genuine transient makes the fast follower
        // shoot up well above the slow one; a steady/sustained signal keeps them close together.
        envFast += (rectified-envFast) * (rectified>envFast? 0.6f : 0.05f);
        envSlow += (rectified-envSlow) * 0.002f;
        timeSinceLastTrigger += 1.0/sr;
        bool onset = (envFast > envSlow*2.2f + 0.01f) && (timeSinceLastTrigger>kMinRetriggerGapSec);
        if (onset) { elapsed=0.0; phase=0.0; timeSinceLastTrigger=0.0; uiJustTriggered.store(true); }
        else if (elapsed<1000.0) elapsed += 1.0/sr;

        float rateHz=currentRateFor(elapsed);
        phase += rateHz/sr; if (phase>=1.0) phase-=std::floor(phase);
        float lfo=lfoWave((float)phase,shp);
        float lfoUni=0.5f*(lfo+1.f);

        if (tgt==Target::Amplitude) {
            float gain=1.f-dep+dep*lfoUni;
            L[i]*=gain; if (R) R[i]*=gain;
        } else if (tgt==Target::Filter) {
            constexpr float minC=300.f, maxC=8000.f;
            float modC=minC*std::pow(maxC/minC,lfoUni);
            float cutoff=juce::jmap(dep,20000.f,modC);
            float a=std::exp(-2.f*juce::MathConstants<float>::pi*cutoff/(float)sr);
            filterStateL=(1.f-a)*L[i]+a*filterStateL; L[i]=filterStateL;
            if (R) { filterStateR=(1.f-a)*R[i]+a*filterStateR; R[i]=filterStateR; }
        } else { // Pitch: modulated delay line (vibrato)
            constexpr float baseMs=15.f, maxModMs=8.f;
            float delayMs=baseMs+dep*maxModMs*lfo;
            float delaySamples=juce::jlimit(1.f,(float)kPitchDelayBufSize-2.f, delayMs*0.001f*(float)sr);
            auto readInterp=[&](std::array<float,kPitchDelayBufSize>& buf,float in_)->float{
                buf[(size_t)pitchWriteIdx]=in_;
                float readPos=(float)pitchWriteIdx-delaySamples;
                while (readPos<0.f) readPos+=(float)kPitchDelayBufSize;
                int i0=(int)readPos; int i1=(i0+1)%kPitchDelayBufSize; float frac=readPos-(float)i0;
                return buf[(size_t)i0]*(1.f-frac)+buf[(size_t)i1]*frac;
            };
            L[i]=readInterp(pitchDelayL,L[i]);
            if (R) R[i]=readInterp(pitchDelayR,R[i]);
            pitchWriteIdx=(pitchWriteIdx+1)%kPitchDelayBufSize;
        }
    }
    uiCurrentRateHz.store(currentRateFor(elapsed));
    uiElapsedSinceTrigger.store((float)elapsed);
}

juce::AudioProcessorEditor* PDAudioProcessor::createEditor() { return new PDAudioProcessorEditor(*this); }

void PDAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::MemoryOutputStream o(destData,true);
    o.writeInt(0x50444331); // "PDC1"
    o.writeBool(bpmSync.load());
    o.writeInt(startDivIndex.load()); o.writeInt(endDivIndex.load());
    o.writeFloat(startRateHz.load()); o.writeFloat(endRateHz.load());
    o.writeFloat(decelTimeSec.load());
    o.writeInt((int)decelCurve.load());
    o.writeFloat(depth.load());
    o.writeInt((int)target.load());
    o.writeInt((int)shape.load());
}
void PDAudioProcessor::setStateInformation(const void* data, int size) {
    juce::MemoryInputStream in(data,(size_t)size,false);
    if (in.readInt()!=0x50444331) return;
    bpmSync.store(in.readBool());
    startDivIndex.store(in.readInt()); endDivIndex.store(in.readInt());
    startRateHz.store(in.readFloat()); endRateHz.store(in.readFloat());
    decelTimeSec.store(in.readFloat());
    decelCurve.store((DecelCurve)in.readInt());
    depth.store(in.readFloat());
    target.store((Target)in.readInt());
    shape.store((Shape)in.readInt());
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PDAudioProcessor(); }
