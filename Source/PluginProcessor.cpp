#include "PluginProcessor.h"
#include "PluginEditor.h"

PDAudioProcessor::PDAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
                                       .withOutput("Output",juce::AudioChannelSet::stereo(),true)),
      apvts(*this,nullptr,"PD_PARAMS",createParameterLayout())
{}

PDAudioProcessor::~PDAudioProcessor(){}

juce::AudioProcessorValueTreeState::ParameterLayout PDAudioProcessor::createParameterLayout(){
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;
    ps.push_back(std::make_unique<juce::AudioParameterBool>(pBypass,"Bypass",false));
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pPattern,"Pattern",
        juce::StringArray{"Pattern 1","Pattern 2","Pattern 3"},0));
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pVolumeCurve,"Volume Curve",
        juce::StringArray{"Curve 1","Curve 2","Curve 3"},0));
    ps.push_back(std::make_unique<juce::AudioParameterFloat>(pGrainMs,"Grain Length",
        juce::NormalisableRange<float>(20.f,300.f,1.f),90.f));
    return { ps.begin(), ps.end() };
}

bool PDAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainInputChannelSet()==juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet()==juce::AudioChannelSet::stereo();
}

void PDAudioProcessor::prepareToPlay(double sampleRate,int){
    sr=sampleRate;
    envFast=0.f; envSlow=0.f; samplesSinceLastTrigger=1LL<<40;
    grainBufferCapacitySamples=(int)std::round(sr*8.0); // 8s - generous headroom over any realistic measure length
    grainBuffer.setSize(2,grainBufferCapacitySamples); grainBuffer.clear();
    grainWritePos=0; grainArmed=false;
    pendingTaps.clear(); activeTaps.clear();
    samplePosition=0;
}

// FIX (redesign): positions are a fraction of ONE MEASURE (0..1), extracted directly from the
// reference audio at exactly one measure @ 75 BPM (27 taps, dense at the start, spreading out
// towards the end of the bar) - dividing the earlier per-beat numbers by 4 (4 beats/measure).
const PDAudioProcessor::Pattern& PDAudioProcessor::getPattern(int index){
    static Pattern patterns[3];
    static bool built=false;
    if(!built){
        built=true;
        float p0[]={0.020f,0.047f,0.062f,0.091f,0.108f,0.125f,0.143f,0.166f,0.187f,0.209f,0.231f,0.253f,
                    0.279f,0.313f,0.337f,0.372f,0.405f,0.445f,0.485f,0.529f,0.589f,0.640f,0.697f,0.747f,
                    0.822f,0.892f,0.970f};
        patterns[0].count=(int)(sizeof(p0)/sizeof(p0[0]));
        for(int i=0;i<patterns[0].count;++i) patterns[0].positions[i]=p0[i];
        // Patterns 2 & 3 (reverse / slower 2-measure variants) are planned but not built yet - empty
        // pattern = no taps added (dry-only), rather than silently reusing Pattern 1 and pretending
        // to be a different option.
        patterns[1].count=0; patterns[2].count=0;
    }
    index=juce::jlimit(0,2,index);
    return patterns[(size_t)index];
}

// FIX (requested): the volume-edit preset must start FLAT, not sloping immediately - an explicit
// plateau from x=0 to the second point, THEN the dip/rise shape, so the first few (densest, closest-
// to-the-attack) taps keep full volume rather than already fading before the "cloud" texture even
// gets going.
PDAudioProcessor::VolumeCurve& PDAudioProcessor::getVolumeCurve(int index){
    static VolumeCurve curves[3];
    static bool built=false;
    if(!built){
        built=true;
        VolumePoint c0[]={ {0.00f,1.00f}, {0.15f,1.00f}, {0.40f,0.55f}, {0.62f,0.50f}, {0.80f,0.82f}, {1.00f,0.95f} };
        curves[0].count=(int)(sizeof(c0)/sizeof(c0[0]));
        for(int i=0;i<curves[0].count;++i) curves[0].points[i]=c0[i];
        // Curves 2 & 3 (other editable fade shapes) planned but not built yet - default to a flat
        // line at unity gain (i.e. "no fade") rather than silently duplicating Curve 1.
        curves[1].count=2; curves[1].points[0]={0.f,1.f}; curves[1].points[1]={1.f,1.f};
        curves[2].count=2; curves[2].points[0]={0.f,1.f}; curves[2].points[1]={1.f,1.f};
    }
    index=juce::jlimit(0,2,index);
    return curves[(size_t)index];
}

float PDAudioProcessor::evalVolumeCurve(const VolumeCurve& c,float x){
    if(c.count<=0) return 1.f;
    if(c.count==1) return c.points[0].y;
    x=juce::jlimit(0.f,1.f,x);
    if(x<=c.points[0].x) return c.points[0].y;
    if(x>=c.points[c.count-1].x) return c.points[c.count-1].y;
    for(int i=0;i<c.count-1;++i){
        if(x>=c.points[i].x && x<=c.points[i+1].x){
            float span=c.points[i+1].x-c.points[i].x;
            float t = span>1e-6f ? (x-c.points[i].x)/span : 0.f;
            float t2 = t*t*t*(t*(t*6.f-15.f)+10.f); // smootherstep - eases in/out of each segment
            return c.points[i].y + (c.points[i+1].y-c.points[i].y)*t2;
        }
    }
    return c.points[c.count-1].y;
}

void PDAudioProcessor::scheduleTapsForTrigger(juce::int64 triggerSample){
    pendingTaps.clear(); activeTaps.clear(); // a fresh note re-arms the whole pattern from scratch
    int patternIdx=(int)apvts.getRawParameterValue(pPattern)->load();
    int curveIdx=(int)apvts.getRawParameterValue(pVolumeCurve)->load();
    const auto& pat=getPattern(patternIdx);
    const auto& curve=getVolumeCurve(curveIdx);
    double bpm=uiBpm.load(); int num=uiTimeSigNumerator.load();
    double measureSec = (60.0/juce::jmax(1.0,bpm)) * juce::jmax(1,num);
    juce::int64 measureSamples=(juce::int64)std::round(measureSec*sr);
    for(int i=0;i<pat.count;++i){
        juce::int64 off=(juce::int64)std::round((double)pat.positions[i]*(double)measureSamples);
        float gain=evalVolumeCurve(curve,pat.positions[i]);
        pendingTaps.push_back({triggerSample+off,gain,triggerSample});
    }
    grainWritePos=0; grainArmed=true; grainBuffer.clear();
}

void PDAudioProcessor::processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&){
    juce::ScopedNoDenormals noDenormals;
    const int n=b.getNumSamples(); const int ch=juce::jmin(2,b.getNumChannels());
    if(ch<2){ samplePosition+=n; return; } // mono-in not supported by this effect's stereo grain design

    // Tempo/time-signature from host, once per block - a plain default (120 BPM, 4/4) if the host
    // doesn't report a tempo (e.g. a bare test host), so the plugin never divides by zero/garbage.
    if(auto* ph=getPlayHead()){
        if(auto pos=ph->getPosition()){
            if(auto bpmOpt=pos->getBpm()) uiBpm.store(*bpmOpt>1.0?*bpmOpt:120.0);
            if(auto tsOpt=pos->getTimeSignature()) uiTimeSigNumerator.store(juce::jmax(1,tsOpt->numerator));
        }
    }

    const bool byp=apvts.getRawParameterValue(pBypass)->load()>0.5f;
    const float grainMs=apvts.getRawParameterValue(pGrainMs)->load();
    const int desiredGrainSamples=grainLengthSamplesFor(grainMs);

    for(int i=0;i<n;++i){
        float L=b.getSample(0,i), R=b.getSample(1,i);
        float mono=0.5f*(L+R);

        // --- Onset (transient) detection - unchanged fast/slow envelope trigger from the original PD ---
        float av=std::abs(mono);
        envFast += (av>envFast? 0.6f:0.05f)*(av-envFast);
        envSlow += (av>envSlow? 0.002f:0.002f)*(av-envSlow);
        ++samplesSinceLastTrigger;
        bool trigger = !byp && envFast>envSlow*2.2f+0.01f && samplesSinceLastTrigger>(int)(0.05*sr);
        if(trigger){
            samplesSinceLastTrigger=0;
            uiElapsedSinceTrigger.store(0.f);
            uiTriggerCount.fetch_add(1);
            scheduleTapsForTrigger(samplePosition+i);
        }

        // --- Grain capture: records the input untouched, starting the instant a trigger fires, so
        // every tap plays back real captured audio, never a synthesized substitute ---
        if(grainArmed && grainWritePos<grainBufferCapacitySamples){
            grainBuffer.setSample(0,grainWritePos,L); grainBuffer.setSample(1,grainWritePos,R);
            ++grainWritePos;
        }

        // --- Activate any pending tap whose scheduled time has arrived ---
        juce::int64 absSample=samplePosition+i;
        for(size_t t=0;t<pendingTaps.size();){
            if(pendingTaps[t].startSample<=absSample){
                // FIX (requested): a tap scheduled very soon after the trigger can only ever replay
                // whatever has actually been captured so far - never more. This isn't a workaround,
                // it naturally produces shorter, tighter early repeats and fuller later ones, which
                // is exactly the texture the reference audio has.
                int availableSoFar=(int)(absSample-pendingTaps[t].triggerSample);
                int len=juce::jlimit(0,desiredGrainSamples,availableSoFar);
                if(len>0) activeTaps.push_back({0,len,len,pendingTaps[t].gain});
                pendingTaps.erase(pendingTaps.begin()+(long)t);
            } else ++t;
        }

        // --- The ORIGINAL signal passes through completely dry and untouched - this line is the
        // whole reason the attack is always preserved: nothing above this point has modified L/R. ---
        float outL=L, outR=R;

        // --- Mix in every currently-active tap's grain playback, with a short raised-cosine fade at
        // both ends of each tap so starting/stopping playback never clicks ---
        for(size_t t=0;t<activeTaps.size();){
            auto& tap=activeTaps[t];
            if(tap.grainReadPos<grainWritePos && tap.samplesRemaining>0){
                float fade=1.f;
                int fadeLen=juce::jmin(kFadeSamples,tap.totalSamples/2);
                int posIntoTap=tap.totalSamples-tap.samplesRemaining;
                if(fadeLen>0){
                    if(posIntoTap<fadeLen) fade=0.5f-0.5f*std::cos(juce::MathConstants<float>::pi*(float)posIntoTap/(float)fadeLen);
                    else if(tap.samplesRemaining<=fadeLen) fade=0.5f-0.5f*std::cos(juce::MathConstants<float>::pi*(float)tap.samplesRemaining/(float)fadeLen);
                }
                float gL=grainBuffer.getSample(0,tap.grainReadPos), gR=grainBuffer.getSample(1,tap.grainReadPos);
                outL += gL*tap.gain*fade; outR += gR*tap.gain*fade;
                ++tap.grainReadPos; --tap.samplesRemaining;
            }
            if(tap.samplesRemaining<=0) activeTaps.erase(activeTaps.begin()+(long)t); else ++t;
        }

        b.setSample(0,i,outL); b.setSample(1,i,outR);
    }

    uiElapsedSinceTrigger.store(uiElapsedSinceTrigger.load()+(float)n/(float)sr);
    samplePosition+=n;
}

juce::AudioProcessorEditor* PDAudioProcessor::createEditor(){ return new PDAudioProcessorEditor(*this); }

void PDAudioProcessor::getStateInformation(juce::MemoryBlock& destData){
    auto state=apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml,destData);
}
void PDAudioProcessor::setStateInformation(const void* data,int sizeInBytes){
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data,sizeInBytes));
    if(xml && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){ return new PDAudioProcessor(); }
