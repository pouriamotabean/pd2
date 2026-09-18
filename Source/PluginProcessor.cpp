#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstring>

PDAudioProcessor::VolumeCurve PDAudioProcessor::getVolumeCurveSnapshot(int curveIdx) const {
    const juce::SpinLock::ScopedLockType sl(volumeCurvesLock);
    return volumeCurves[juce::jlimit(0,2,curveIdx)]; // small POD struct - cheap to copy while locked
}
void PDAudioProcessor::setVolumeCurvePointY(int curveIdx, int pointIdx, float y){
    const juce::SpinLock::ScopedLockType sl(volumeCurvesLock);
    auto& c=volumeCurves[juce::jlimit(0,2,curveIdx)];
    if(pointIdx>=0 && pointIdx<c.count) c.points[pointIdx].y=y;
}
PDAudioProcessor::PDAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
                                       .withOutput("Output",juce::AudioChannelSet::stereo(),true)),
      apvts(*this,nullptr,"PD_PARAMS",createParameterLayout())
{
    for(int i=0;i<3;++i) volumeCurves[i]=defaultVolumeCurve(i);
}

PDAudioProcessor::~PDAudioProcessor(){}

juce::AudioProcessorValueTreeState::ParameterLayout PDAudioProcessor::createParameterLayout(){
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;
    ps.push_back(std::make_unique<juce::AudioParameterBool>(pBypass,"Bypass",false));
    // FIX (requested): "Pattern 1/2/3" were meaningless placeholders from before the pattern itself
    // was finalized. Now that it is, this same parameter becomes the real preset switch: Forward (the
    // tuned build-up-then-rest pattern) and Reverse (the same shape mirrored - rest, then build into
    // the next downbeat). Two real, named choices instead of three placeholder ones.
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pPattern,"Pattern",
        juce::StringArray{"Forward","Reverse"},0));
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

// FIX (requested): the tail's last 6 points are now the EXACT positions read directly from the
// reference Cubase project's MIDI notes (bar.beat.16th.tick, confirmed via screenshot, converted at
// 480 ticks/quarter, 4/4): 26.2.4.96, 26.3.2.1, 26.3.3.46, 26.3.4.113, 26.4.2.87, 26.4.4.87 -> as
// fractions of the measure: 0.4875, 0.5630, 0.6490, 0.7464, 0.8578, 0.9828. This replaces an earlier
// estimate (extended from a smooth formula, then a separate mp3-based automated peak-detection guess)
// with ground truth - no more guessing at this part of the pattern. Notably the reference's last note
// lands at 98.3% of the measure (~34ms of trailing space at 120 BPM) - much closer to the boundary
// than an earlier mp3-based estimate suggested, which was very likely a peak-detection undercount.
static PDAudioProcessor::Pattern buildForwardPattern(){
    PDAudioProcessor::Pattern pat{};
    float p0[]={0.000f,0.007f,0.015f,0.024f,0.034f,0.047f,0.061f,0.078f,0.098f,0.121f,0.147f,0.178f,
                0.214f,0.256f,0.305f,0.362f,0.429f,
                0.4875f,0.5630f,0.6490f,0.7464f,0.8578f,0.9828f};
    pat.count=(int)(sizeof(p0)/sizeof(p0[0]));
    for(int i=0;i<pat.count;++i) pat.positions[i]=p0[i];
    return pat;
}
// FIX (requested - Preset 2, "Reverse"): the exact mirror image of Forward, reflected around the
// centre of the measure AND reversed in order (reversed[i] = 1 - forward[N-1-i]). Forward is dense at
// the start and rests before the end; Reverse rests at the start and builds to maximum density right
// into the next downbeat - a classic riser/build shape. Computed FROM Forward rather than hand-tuned
// separately, so any future adjustment to Forward's tail automatically keeps Reverse's opening
// consistent with it, instead of the two silently drifting apart over time.
static PDAudioProcessor::Pattern buildReversePattern(){
    const auto& fwd=buildForwardPattern();
    PDAudioProcessor::Pattern pat{};
    pat.count=fwd.count;
    for(int i=0;i<fwd.count;++i){
        float mirrored=1.0f-fwd.positions[fwd.count-1-i];
        // Safety margin identical in spirit to Forward's own - never land exactly ON the next
        // measure's first tap (fraction 0.0), which would double-trigger at the same instant.
        pat.positions[i]=juce::jmin(mirrored,0.985f);
    }
    return pat;
}
const PDAudioProcessor::Pattern& PDAudioProcessor::getPattern(int index){
    // FIX (real bug found in review, still applies): a static local with a real initializer
    // expression is "magic statics" - the C++11 standard itself guarantees this runs exactly once,
    // thread-safe, no matter how many threads call this concurrently. No manual flag/lock needed.
    static const Pattern forward = buildForwardPattern();
    static const Pattern reverse = buildReversePattern();
    return (juce::jlimit(0,1,index)==0) ? forward : reverse;
}

// FIX (requested): the factory default must be a genuinely FLAT line - every point at gain 1.0, no
// pre-baked dip/rise shape. This is a MANUAL curve the user shapes themselves; starting with an
// opinionated wavy shape already on it isn't a neutral starting point.
PDAudioProcessor::VolumeCurve PDAudioProcessor::defaultVolumeCurve(int index){
    juce::ignoreUnused(index);
    VolumeCurve c;
    c.count=6;
    float xs[]={0.00f,0.15f,0.40f,0.62f,0.80f,1.00f};
    for(int i=0;i<6;++i) c.points[i]={xs[i],1.0f};
    return c;
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
    pendingTaps.clear();
    // FIX (requested #3): don't hard-clear active taps - snapshot each one's next few milliseconds of
    // audio out of the CURRENT grain buffer (which is about to be reused/cleared for this new note)
    // into its own small private tail, and switch it into a forced linear fade-to-zero. The mixing
    // loop in processBlock() reads from this tail instead of the (now-repurposed) grain buffer once
    // forced==true, so the interrupted tap keeps sounding, just fading out quickly, instead of
    // vanishing on the next sample.
    for(auto& tap:activeTaps){
        if(tap.forced) continue; // already fading from an earlier interruption - leave it be
        tap.forced=true;
        tap.forcedTotal=juce::jmin(kForceFadeSamples,tap.samplesRemaining);
        for(int k=0;k<tap.forcedTotal;++k){
            int srcPos=tap.grainReadPos+k;
            tap.tailL[(size_t)k]= srcPos<grainWritePos ? grainBuffer.getSample(0,srcPos) : 0.f;
            tap.tailR[(size_t)k]= srcPos<grainWritePos ? grainBuffer.getSample(1,srcPos) : 0.f;
        }
        tap.grainReadPos=0; tap.samplesRemaining=tap.forcedTotal; tap.totalSamples=tap.forcedTotal;
    }
    int patternIdx=(int)apvts.getRawParameterValue(pPattern)->load();
    int curveIdx=(int)apvts.getRawParameterValue(pVolumeCurve)->load();
    const auto& pat=getPattern(patternIdx);
    const auto curve=getVolumeCurveSnapshot(curveIdx); // thread-safe copy, not a reference - see header
    double bpm=uiBpm.load(); int num=uiTimeSigNumerator.load();
    double measureSec = (60.0/juce::jmax(1.0,bpm)) * juce::jmax(1,num);
    juce::int64 measureSamples=(juce::int64)std::round(measureSec*sr);
    for(int i=0;i<pat.count;++i){
        juce::int64 off=(juce::int64)std::round((double)pat.positions[i]*(double)measureSamples);
        float gain=evalVolumeCurve(curve,pat.positions[i]);
        pendingTaps.push_back({triggerSample+off,gain,triggerSample,0});
    }
    // FIX (requested - "most important problem"): each tap's playback is now also capped by the gap
    // to the NEXT scheduled tap, not just by the Grain Length parameter. Without this, closely-spaced
    // early taps (as little as 15-20ms apart) each played up to the full ~90ms grain length, so 4-5
    // consecutive taps' playback heavily overlapped and blurred into one smeared burst instead of a
    // clean, distinct repeat texture. Capped here, at schedule time, since it depends on knowing
    // where the *next* tap sits, which isn't available yet when a tap is first activated.
    for(size_t i=0;i<pendingTaps.size();++i){
        if(i+1<pendingTaps.size()){
            juce::int64 gap=pendingTaps[i+1].startSample-pendingTaps[i].startSample;
            pendingTaps[i].maxLenSamples=juce::jmax((juce::int64)1,gap);
        } else {
            pendingTaps[i].maxLenSamples=1LL<<40; // last tap in the pattern - no next tap to worry about
        }
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
        envFast += (av>envFast? 0.6f:0.05f)*(av-envFast); // genuinely asymmetric: fast attack, slower release - this is what makes it "fast"
        envSlow += 0.002f*(av-envSlow); // FIX (code cleanliness - real issue found in review): this used
        // to be written as (av>envSlow? 0.002f:0.002f) - a ternary whose two branches were identical,
        // so the condition never did anything. envSlow is meant to track a slow, symmetric baseline
        // (unlike envFast above, which is deliberately asymmetric) - not a bug, but the old ternary
        // read like an unfinished attack/release split that was never actually implemented, which was
        // genuinely confusing to anyone reading it afterwards. This says what it actually does.
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
                juce::int64 cap64=juce::jmin((juce::int64)desiredGrainSamples,pendingTaps[t].maxLenSamples);
                int len=juce::jlimit(0,(int)cap64,availableSoFar);
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
            if(tap.forced){
                // FIX (requested #3): forced (interrupted) tap - read from its own private tail
                // snapshot, never the (now possibly-repurposed) main grain buffer, and always ramp
                // linearly to exactly zero by the end, regardless of how loud it was when interrupted.
                if(tap.samplesRemaining>0){
                    int posIntoTap=tap.forcedTotal-tap.samplesRemaining;
                    float ramp = tap.forcedTotal>0 ? 1.f-(float)posIntoTap/(float)tap.forcedTotal : 0.f;
                    outL += tap.tailL[(size_t)tap.grainReadPos]*tap.gain*ramp;
                    outR += tap.tailR[(size_t)tap.grainReadPos]*tap.gain*ramp;
                    ++tap.grainReadPos; --tap.samplesRemaining;
                }
                if(tap.samplesRemaining<=0) activeTaps.erase(activeTaps.begin()+(long)t); else ++t;
                continue;
            }
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
    juce::MemoryBlock curvesBlock;
    {
        const juce::SpinLock::ScopedLockType sl(volumeCurvesLock);
        for(int c=0;c<3;++c){
            curvesBlock.append(&volumeCurves[c].count,sizeof(int));
            curvesBlock.append(volumeCurves[c].points,sizeof(VolumePoint)*kMaxVolumePoints);
        }
    }
    state.setProperty("volumeCurvesData",curvesBlock.toBase64Encoding(),nullptr);
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml,destData);
}
void PDAudioProcessor::setStateInformation(const void* data,int sizeInBytes){
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data,sizeInBytes));
    if(!xml || !xml->hasTagName(apvts.state.getType())) return;
    auto tree=juce::ValueTree::fromXml(*xml);
    apvts.replaceState(tree);
    const size_t perCurve=sizeof(int)+sizeof(VolumePoint)*kMaxVolumePoints;
    juce::MemoryBlock mb;
    bool ok = tree.hasProperty("volumeCurvesData")
        && mb.fromBase64Encoding(tree["volumeCurvesData"].toString())
        && mb.getSize()>=perCurve*3;
    const juce::SpinLock::ScopedLockType sl(volumeCurvesLock);
    for(int c=0;c<3;++c){
        if(ok){
            size_t pos=c*perCurve;
            int count=0; std::memcpy(&count,(const char*)mb.getData()+pos,sizeof(int));
            volumeCurves[c].count=juce::jlimit(0,kMaxVolumePoints,count);
            std::memcpy(volumeCurves[c].points,(const char*)mb.getData()+pos+sizeof(int),sizeof(VolumePoint)*kMaxVolumePoints);
        } else {
            // Older save with no curve data, or a corrupt block - fall back to the flat factory
            // default rather than leaving whatever was in memory before this call.
            volumeCurves[c]=defaultVolumeCurve(c);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){ return new PDAudioProcessor(); }
