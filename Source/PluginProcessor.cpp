#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstring>
#include <algorithm>

PDAudioProcessor::PDAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
                                       .withOutput("Output",juce::AudioChannelSet::stereo(),true)),
      apvts(*this,nullptr,"PD_PARAMS",createParameterLayout())
{
    // FIX (requested): Forward/Reverse now start from the tuned rhythmic shape (positions fixed by
    // editor policy, values editable) instead of being fully immutable - both buffers of each pair
    // are seeded identically so it doesn't matter which one starts "active". Custom's buffers are
    // left at their default count=0 (an empty pattern) - the spec's required starting state.
    const auto fwdSeed=buildForwardSeed();
    const auto revSeed=buildReverseSeed();
    presetBuffers[(size_t)kPresetForward][0]=fwdSeed; presetBuffers[(size_t)kPresetForward][1]=fwdSeed;
    presetBuffers[(size_t)kPresetReverse][0]=revSeed; presetBuffers[(size_t)kPresetReverse][1]=revSeed;
    for(auto& a:presetActiveBuffer) a.store(0);
}

PDAudioProcessor::~PDAudioProcessor(){}

juce::AudioProcessorValueTreeState::ParameterLayout PDAudioProcessor::createParameterLayout(){
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;
    ps.push_back(std::make_unique<juce::AudioParameterBool>(pBypass,"Bypass",false));
    ps.push_back(std::make_unique<juce::AudioParameterChoice>(pPreset,"Pattern",
        juce::StringArray{"Forward","Reverse","Custom"},0));
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
    grainBufferCapacitySamples=(int)std::round(sr*8.0);
    grainBuffer.setSize(2,grainBufferCapacitySamples); grainBuffer.clear();
    grainWritePos=0; grainArmed=false;
    pendingTaps.clear(); activeTaps.clear();
    samplePosition=0;
}

// Ground-truth positions from the reference project's MIDI (bar.beat.16th.tick, confirmed via
// screenshot). This is now just the SEED that Forward starts from - the editor lets its volumeDb (and
// later pan/pitch/formant) be adjusted afterward; only the positions stay fixed by editor policy.
PDAudioProcessor::RepeatPattern PDAudioProcessor::buildForwardSeed(){
    RepeatPattern pat{};
    float p0[]={0.000f,0.007f,0.015f,0.024f,0.034f,0.047f,0.061f,0.078f,0.098f,0.121f,0.147f,0.178f,
                0.214f,0.256f,0.305f,0.362f,0.429f,
                0.4875f,0.5630f,0.6490f,0.7464f,0.8578f,0.9828f};
    pat.count=(int)(sizeof(p0)/sizeof(p0[0]));
    for(int i=0;i<pat.count;++i){
        auto& e=pat.events[i];
        e=RepeatEvent{}; // defaults (volumeDb=0 etc)
        e.id=i; e.position=p0[i];
    }
    return pat;
}
// Reverse: the exact mirror of Forward (reflected around the centre of the measure, order reversed) -
// computed FROM Forward so the two never silently drift apart.
PDAudioProcessor::RepeatPattern PDAudioProcessor::buildReverseSeed(){
    const auto fwd=buildForwardSeed();
    RepeatPattern pat{};
    pat.count=fwd.count;
    for(int i=0;i<fwd.count;++i){
        auto& e=pat.events[i];
        e=fwd.events[fwd.count-1-i];
        e.id=i;
        e.position=juce::jmin(1.0f-fwd.events[fwd.count-1-i].position,1.0f)*0.985f;
    }
    return pat;
}

// ---- Per-preset lock-free double-buffer exchange (spec section 23) ----
PDAudioProcessor::RepeatPattern PDAudioProcessor::getPatternForEditing(int presetIndex) const {
    int idx=juce::jlimit(0,2,presetIndex);
    return presetBuffers[(size_t)idx][(size_t)presetActiveBuffer[(size_t)idx].load()];
}
void PDAudioProcessor::commitPattern(int presetIndex, const RepeatPattern& newPattern){
    int idx=juce::jlimit(0,2,presetIndex);
    int activeIdx=presetActiveBuffer[(size_t)idx].load();
    int writeIdx=1-activeIdx;
    presetBuffers[(size_t)idx][(size_t)writeIdx]=newPattern; // fully write the INACTIVE buffer first...
    presetActiveBuffer[(size_t)idx].store(writeIdx);         // ...then atomically publish it.
}
const PDAudioProcessor::RepeatPattern& PDAudioProcessor::getActivePatternForAudio(int presetIndex) const {
    int idx=juce::jlimit(0,2,presetIndex);
    return presetBuffers[(size_t)idx][(size_t)presetActiveBuffer[(size_t)idx].load()];
}

void PDAudioProcessor::scheduleTapsForTrigger(juce::int64 triggerSample){
    pendingTaps.clear();
    for(auto& tap:activeTaps){
        if(tap.forced) continue;
        tap.forced=true;
        tap.forcedTotal=juce::jmin(kForceFadeSamples,tap.samplesRemaining);
        for(int k=0;k<tap.forcedTotal;++k){
            int srcPos=tap.grainReadPos+k;
            tap.tailL[(size_t)k]= srcPos<grainWritePos ? grainBuffer.getSample(0,srcPos) : 0.f;
            tap.tailR[(size_t)k]= srcPos<grainWritePos ? grainBuffer.getSample(1,srcPos) : 0.f;
        }
        tap.grainReadPos=0; tap.samplesRemaining=tap.forcedTotal; tap.totalSamples=tap.forcedTotal;
    }

    int presetIdx=(int)apvts.getRawParameterValue(pPreset)->load();
    const auto& pat=getActivePatternForAudio(presetIdx);

    double bpm=uiBpm.load(); int num=uiTimeSigNumerator.load();
    double measureSec = (60.0/juce::jmax(1.0,bpm)) * juce::jmax(1,num);
    juce::int64 measureSamples=(juce::int64)std::round(measureSec*sr);

    struct Sortable { float position; float gain; float pan; };
    std::vector<Sortable> sorted;
    sorted.reserve((size_t)pat.count);
    for(int i=0;i<pat.count;++i){
        const auto& e=pat.events[i];
        if(!e.enabled) continue;
        float gain=juce::Decibels::decibelsToGain(juce::jlimit(-12.f,12.f,e.volumeDb));
        sorted.push_back({juce::jlimit(0.f,1.f,e.position),gain,juce::jlimit(-1.f,1.f,e.pan)});
    }
    std::sort(sorted.begin(),sorted.end(),[](const Sortable&a,const Sortable&b){return a.position<b.position;});

    for(auto& s:sorted){
        juce::int64 off=(juce::int64)std::round((double)s.position*(double)measureSamples);
        pendingTaps.push_back({triggerSample+off,s.gain,s.pan,triggerSample,0});
    }
    for(size_t i=0;i<pendingTaps.size();++i){
        if(i+1<pendingTaps.size()){
            juce::int64 gap=pendingTaps[i+1].startSample-pendingTaps[i].startSample;
            pendingTaps[i].maxLenSamples=juce::jmax((juce::int64)1,gap);
        } else {
            pendingTaps[i].maxLenSamples=1LL<<40;
        }
    }
    grainWritePos=0; grainArmed=true; grainBuffer.clear();
}

void PDAudioProcessor::processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer&){
    juce::ScopedNoDenormals noDenormals;
    const int n=b.getNumSamples(); const int ch=juce::jmin(2,b.getNumChannels());
    if(ch<2){ samplePosition+=n; return; }

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

        float av=std::abs(mono);
        envFast += (av>envFast? 0.6f:0.05f)*(av-envFast);
        envSlow += 0.002f*(av-envSlow);
        ++samplesSinceLastTrigger;
        bool trigger = !byp && envFast>envSlow*2.2f+0.01f && samplesSinceLastTrigger>(int)(0.05*sr);
        if(trigger){
            samplesSinceLastTrigger=0;
            uiElapsedSinceTrigger.store(0.f);
            uiTriggerCount.fetch_add(1);
            scheduleTapsForTrigger(samplePosition+i);
        }

        if(grainArmed && grainWritePos<grainBufferCapacitySamples){
            grainBuffer.setSample(0,grainWritePos,L); grainBuffer.setSample(1,grainWritePos,R);
            ++grainWritePos;
        }

        juce::int64 absSample=samplePosition+i;
        for(size_t t=0;t<pendingTaps.size();){
            if(pendingTaps[t].startSample<=absSample){
                int availableSoFar=(int)(absSample-pendingTaps[t].triggerSample);
                juce::int64 cap64=juce::jmin((juce::int64)desiredGrainSamples,pendingTaps[t].maxLenSamples);
                int len=juce::jlimit(0,(int)cap64,availableSoFar);
                if(len>0) activeTaps.push_back({0,len,len,pendingTaps[t].gain,pendingTaps[t].pan});
                pendingTaps.erase(pendingTaps.begin()+(long)t);
            } else ++t;
        }

        float outL=L, outR=R;

        for(size_t t=0;t<activeTaps.size();){
            auto& tap=activeTaps[t];
            // FIX (Phase 2 - PAN mode wired to DSP): a simple balance law - panning right fades this
            // tap's contribution to the LEFT channel, panning left fades its contribution to the
            // RIGHT channel, centre (0) leaves both at full gain. Deliberately simple/predictable
            // rather than equal-power, since the grain itself is already a real stereo capture (not a
            // mono source being spread into stereo) - this reads as "balance the repeat" rather than
            // introducing a new stereo image from scratch.
            const float panGainL = tap.pan<=0.f ? 1.f : 1.f-tap.pan;
            const float panGainR = tap.pan>=0.f ? 1.f : 1.f+tap.pan;
            if(tap.forced){
                if(tap.samplesRemaining>0){
                    int posIntoTap=tap.forcedTotal-tap.samplesRemaining;
                    float ramp = tap.forcedTotal>0 ? 1.f-(float)posIntoTap/(float)tap.forcedTotal : 0.f;
                    outL += tap.tailL[(size_t)tap.grainReadPos]*tap.gain*ramp*panGainL;
                    outR += tap.tailR[(size_t)tap.grainReadPos]*tap.gain*ramp*panGainR;
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
                outL += gL*tap.gain*fade*panGainL; outR += gR*tap.gain*fade*panGainR;
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

// FIX (requested): all THREE patterns (Forward/Reverse/Custom) now have editable values that must
// survive project save/reload, not just Custom - each serialized as a simple length-prefixed block of
// RepeatEvents, base64'd into the same APVTS ValueTree the rest of the state already uses.
void PDAudioProcessor::getStateInformation(juce::MemoryBlock& destData){
    auto state=apvts.copyState();
    static const char* keys[3]={"forwardPatternData","reversePatternData","customPatternData"};
    for(int p=0;p<3;++p){
        auto pat=getPatternForEditing(p);
        juce::MemoryBlock block;
        block.append(&pat.count,sizeof(int));
        block.append(pat.events,sizeof(RepeatEvent)*kMaxRepeats);
        state.setProperty(keys[p],block.toBase64Encoding(),nullptr);
    }
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml,destData);
}
void PDAudioProcessor::setStateInformation(const void* data,int sizeInBytes){
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data,sizeInBytes));
    if(!xml || !xml->hasTagName(apvts.state.getType())) return;
    auto tree=juce::ValueTree::fromXml(*xml);
    apvts.replaceState(tree);

    static const char* keys[3]={"forwardPatternData","reversePatternData","customPatternData"};
    const size_t expected=sizeof(int)+sizeof(RepeatEvent)*kMaxRepeats;
    for(int p=0;p<3;++p){
        RepeatPattern loaded{};
        juce::MemoryBlock mb;
        bool ok = tree.hasProperty(keys[p])
            && mb.fromBase64Encoding(tree[keys[p]].toString())
            && mb.getSize()>=expected;
        if(ok){
            int count=0; std::memcpy(&count,mb.getData(),sizeof(int));
            loaded.count=juce::jlimit(0,kMaxRepeats,count);
            std::memcpy(loaded.events,(const char*)mb.getData()+sizeof(int),sizeof(RepeatEvent)*kMaxRepeats);
        } else {
            // Older save (pre-this-feature) or nothing built yet - fall back to the tuned seed for
            // Forward/Reverse (never an empty pattern for those two), or a genuinely empty Custom.
            if(p==(int)kPresetForward) loaded=buildForwardSeed();
            else if(p==(int)kPresetReverse) loaded=buildReverseSeed();
        }
        commitPattern(p,loaded);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){ return new PDAudioProcessor(); }
