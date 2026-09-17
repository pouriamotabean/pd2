#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {
constexpr auto pBypass = "bypass";
constexpr auto pSync = "sync";
constexpr auto pStartDiv = "startDiv";
constexpr auto pEndDiv = "endDiv";
constexpr auto pStartHz = "startHz";
constexpr auto pEndHz = "endHz";
constexpr auto pDecel = "decel";
constexpr auto pCurve = "curve";
constexpr auto pDepth = "depth";
constexpr auto pTarget = "target";
constexpr auto pShape = "shape";
}

juce::AudioProcessorValueTreeState::ParameterLayout PDAudioProcessor::createParameterLayout() {
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;
    ps.push_back(std::make_unique<juce::AudioParameterBool>(pBypass, "Bypass", false));
    ps.push_back(std::make_unique<juce::AudioParameterBool>(pSync, "Tempo Sync", true));

    juce::StringArray divs;
    for (auto* n : kDivisionNames) divs.add(n);
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pStartDiv, "Start Division", divs, 1));
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pEndDiv, "End Division", divs, 3));
    ps.push_back(std::make_unique<juce::AudioParameterFloat>(pStartHz, "Start Rate", juce::NormalisableRange<float>(0.5f,20.f,0.01f), 8.f, "Hz"));
    ps.push_back(std::make_unique<juce::AudioParameterFloat>(pEndHz, "End Rate", juce::NormalisableRange<float>(0.5f,20.f,0.01f), 2.f, "Hz"));
    ps.push_back(std::make_unique<juce::AudioParameterFloat>(pDecel, "Deceleration", juce::NormalisableRange<float>(0.1f,10.f,0.01f), 2.f, "s"));
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pCurve, "Curve", juce::StringArray{"EXP","LIN","LOG"}, 0));
    ps.push_back(std::make_unique<juce::AudioParameterFloat>(pDepth, "Depth", juce::NormalisableRange<float>(0.f,1.f,0.001f), 0.7f));
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pTarget, "Target", juce::StringArray{"AMPLITUDE","FILTER","PITCH"}, 0));
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pShape, "Shape", juce::StringArray{"SINE","TRIANGLE","SQUARE","SAW"}, 0));
    return { ps.begin(), ps.end() };
}

PDAudioProcessor::PDAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PD_PARAMS", createParameterLayout())
{
    syncAtomicsFromParameters();
}

bool PDAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

void PDAudioProcessor::prepareToPlay(double sampleRate, int) {
    sr = sampleRate;
    envFast = envSlow = detectorFloor = 0.f;
    detectorArmed = true;
    timeSinceLastTrigger = 1.0;
    phase = 0.0;
    elapsed = 999.0;
    filterStateL = filterStateR = 0.f;
    filterCoeff = 0.f;
    pitchDelayL.fill(0.f);
    pitchDelayR.fill(0.f);
    pitchWriteIdx = 0;
    uiCurrentRateHz.store(currentRateFor(elapsed));
    uiElapsedSinceTrigger.store((float)elapsed);
    uiInputLevelDb.store(-100.f);
    uiTriggerCount.store(0);
}

float PDAudioProcessor::shapeCurve(float t, DecelCurve c) {
    t = juce::jlimit(0.f,1.f,t);
    switch (c) {
        case DecelCurve::Exponential: {
            constexpr float k = 5.f;
            return (1.f-std::exp(-k*t))/(1.f-std::exp(-k));
        }
        case DecelCurve::Logarithmic: return t*t;
        case DecelCurve::Linear: default: return t;
    }
}

float PDAudioProcessor::lfoWave(float phase01, Shape s) {
    switch (s) {
        case Shape::Triangle: return 2.f*std::abs(2.f*(phase01-std::floor(phase01+0.5f)))-1.f;
        case Shape::Square: return phase01 < 0.5f ? 1.f : -1.f;
        case Shape::Saw: return 2.f*phase01-1.f;
        case Shape::Sine: default: return std::sin(juce::MathConstants<float>::twoPi*phase01);
    }
}

void PDAudioProcessor::syncAtomicsFromParameters() {
    bypassed.store(apvts.getRawParameterValue(pBypass)->load() > 0.5f);
    bpmSync.store(apvts.getRawParameterValue(pSync)->load() > 0.5f);
    startDivIndex.store((int)std::lround(apvts.getRawParameterValue(pStartDiv)->load()));
    endDivIndex.store((int)std::lround(apvts.getRawParameterValue(pEndDiv)->load()));
    startRateHz.store(apvts.getRawParameterValue(pStartHz)->load());
    endRateHz.store(apvts.getRawParameterValue(pEndHz)->load());
    decelTimeSec.store(apvts.getRawParameterValue(pDecel)->load());
    decelCurve.store((DecelCurve)(int)std::lround(apvts.getRawParameterValue(pCurve)->load()));
    depth.store(apvts.getRawParameterValue(pDepth)->load());
    target.store((Target)(int)std::lround(apvts.getRawParameterValue(pTarget)->load()));
    shape.store((Shape)(int)std::lround(apvts.getRawParameterValue(pShape)->load()));
}

float PDAudioProcessor::currentRateFor(double elapsedSec) const {
    float startHz, endHz;
    if (bpmSync.load()) {
        int si = juce::jlimit(0,kNumDivisions-1,startDivIndex.load());
        int ei = juce::jlimit(0,kNumDivisions-1,endDivIndex.load());
        startHz = (float)(currentBpm/(60.0*kDivisionBeats[si]));
        endHz = (float)(currentBpm/(60.0*kDivisionBeats[ei]));
    } else {
        startHz = startRateHz.load();
        endHz = endRateHz.load();
    }
    float decelTime = juce::jmax(0.05f,decelTimeSec.load());
    float shaped = shapeCurve((float)(elapsedSec/decelTime),decelCurve.load());
    return startHz + (endHz-startHz)*shaped;
}

void PDAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int numCh = buffer.getNumChannels(), numSamples = buffer.getNumSamples();
    if (numCh < 2) return;

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition(); pos.hasValue())
            if (auto bpmOpt = pos->getBpm(); bpmOpt.hasValue())
                currentBpm = juce::jmax(20.0,*bpmOpt);

    // APVTS is the automation/state authority; atomics remain the DSP-facing cache.
    syncAtomicsFromParameters();
    if (bypassed.load()) {
        uiCurrentRateHz.store(currentRateFor(elapsed));
        uiElapsedSinceTrigger.store((float)elapsed);
        return;
    }

    auto* L = buffer.getWritePointer(0);
    auto* R = buffer.getWritePointer(1);
    const Target tgt = target.load();
    const Shape shp = shape.load();
    const float dep = juce::jlimit(0.f,1.f,depth.load());

    float blockPeak = 0.f;
    for (int i=0;i<numSamples;++i) {
        const float in = 0.5f*(L[i]+R[i]);
        blockPeak = juce::jmax(blockPeak,std::abs(in));
        const float rectified = std::abs(in);

        // Faster attack, controlled release. The adaptive floor prevents a sustained loud signal
        // from repeatedly retriggering while a quiet transient can still be detected.
        envFast += (rectified-envFast) * (rectified>envFast ? 0.45f : 0.035f);
        envSlow += (rectified-envSlow) * 0.0015f;
        detectorFloor += (envSlow-detectorFloor) * 0.0025f;
        timeSinceLastTrigger += 1.0/sr;

        const float threshold = juce::jmax(0.008f, detectorFloor*1.35f + 0.006f);
        const bool above = envFast > juce::jmax(envSlow*2.0f + 0.006f, threshold);
        const bool onset = detectorArmed && above && timeSinceLastTrigger > kMinRetriggerGapSec;
        if (onset) {
            elapsed = 0.0;
            phase = 0.0;
            timeSinceLastTrigger = 0.0;
            detectorArmed = false;
            uiJustTriggered.store(true);
            uiTriggerCount.fetch_add(1);
        } else if (!above && envFast < envSlow*1.25f + 0.004f) {
            detectorArmed = true;
        } else if (elapsed < 1000.0) {
            elapsed += 1.0/sr;
        }

        const float rateHz = currentRateFor(elapsed);
        phase += rateHz/sr;
        if (phase >= 1.0) phase -= std::floor(phase);
        const float lfo = lfoWave((float)phase,shp);
        const float lfoUni = 0.5f*(lfo+1.f);

        if (tgt == Target::Amplitude) {
            const float gain = 1.f-dep+dep*lfoUni;
            L[i] *= gain;
            R[i] *= gain;
        } else if (tgt == Target::Filter) {
            constexpr float minC=300.f, maxC=8000.f;
            const float modC=minC*std::pow(maxC/minC,lfoUni);
            const float cutoff=juce::jmap(dep,20000.f,modC);
            const float wanted=std::exp(-2.f*juce::MathConstants<float>::pi*cutoff/(float)sr);
            // Smooth the coefficient itself to reduce zippering/clicks at high modulation rates.
            filterCoeff += (wanted-filterCoeff)*0.08f;
            filterStateL=(1.f-filterCoeff)*L[i]+filterCoeff*filterStateL;
            filterStateR=(1.f-filterCoeff)*R[i]+filterCoeff*filterStateR;
            L[i]=filterStateL;
            R[i]=filterStateR;
        } else {
            // Stable modulated-delay vibrato. This remains a vibrato-style pitch effect rather than
            // a time-domain pitch shifter; the exposed "Pitch" target and its range are unchanged.
            constexpr float baseMs=15.f, maxModMs=8.f;
            const float delayMs=baseMs+dep*maxModMs*lfo;
            const float delaySamples=juce::jlimit(1.f,(float)kPitchDelayBufSize-2.f,delayMs*0.001f*(float)sr);
            pitchDelayL[(size_t)pitchWriteIdx]=L[i];
            pitchDelayR[(size_t)pitchWriteIdx]=R[i];
            auto readInterp=[&](std::array<float,kPitchDelayBufSize>& b)->float {
                float readPos=(float)pitchWriteIdx-delaySamples;
                while (readPos<0.f) readPos+=(float)kPitchDelayBufSize;
                const int i0=(int)readPos;
                const int i1=(i0+1)%kPitchDelayBufSize;
                const float frac=readPos-(float)i0;
                return b[(size_t)i0]*(1.f-frac)+b[(size_t)i1]*frac;
            };
            L[i]=readInterp(pitchDelayL);
            R[i]=readInterp(pitchDelayR);
            pitchWriteIdx=(pitchWriteIdx+1)%kPitchDelayBufSize;
        }
    }

    uiCurrentRateHz.store(currentRateFor(elapsed));
    uiElapsedSinceTrigger.store((float)elapsed);
    uiInputLevelDb.store(juce::Decibels::gainToDecibels(juce::jmax(blockPeak,1.0e-5f)));
}

juce::AudioProcessorEditor* PDAudioProcessor::createEditor() { return new PDAudioProcessorEditor(*this); }

void PDAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml,destData);
}

void PDAudioProcessor::setStateInformation(const void* data, int size) {
    if (auto xml = getXmlFromBinary(data,size)) {
        if (xml->hasTagName(apvts.state.getType())) {
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
            syncAtomicsFromParameters();
            return;
        }
    }

    // Backward-compatible loader for v0.1 PDC1 states.
    juce::MemoryInputStream in(data,(size_t)size,false);
    if (size >= 4 && in.readInt()==0x50444331) {
        auto setFloat=[this](const char* id,float v){ if(auto* q=apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
        auto setChoice=[this](const char* id,int v){ if(auto* q=apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1((float)v)); };
        const bool sync=in.readBool();
        if(auto* q=apvts.getParameter(pSync)) q->setValueNotifyingHost(q->convertTo0to1(sync?1.f:0.f));
        setChoice(pStartDiv,in.readInt()); setChoice(pEndDiv,in.readInt());
        setFloat(pStartHz,in.readFloat()); setFloat(pEndHz,in.readFloat()); setFloat(pDecel,in.readFloat());
        setChoice(pCurve,in.readInt()); setFloat(pDepth,in.readFloat()); setChoice(pTarget,in.readInt()); setChoice(pShape,in.readInt());
        syncAtomicsFromParameters();
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PDAudioProcessor(); }
