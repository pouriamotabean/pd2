#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstring>
#include <algorithm>

PDAudioProcessor::PDAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true)
                                       .withOutput("Output",juce::AudioChannelSet::stereo(),true)),
      apvts(*this,nullptr,"PD_PARAMS",createParameterLayout())
{
    // customBuffers default-construct to count=0 (an empty pattern) - the spec's required "start from
    // an empty pattern" state (section 15/26) is simply what a fresh instance already has, no special
    // case needed.
}

PDAudioProcessor::~PDAudioProcessor(){}

juce::AudioProcessorValueTreeState::ParameterLayout PDAudioProcessor::createParameterLayout(){
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;
    ps.push_back(std::make_unique<juce::AudioParameterBool>(pBypass,"Bypass",false));
    // FIX (Phase 1 of the interactive-editor rewrite): this parameter now selects between the two
    // LOCKED shapes PD already had (Forward/Reverse, kept exactly "in their current form" per the
    // explicit request) and the new fully user-editable Custom pattern.
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
    grainBufferCapacitySamples=(int)std::round(sr*8.0); // 8s - generous headroom over any realistic measure length
    grainBuffer.setSize(2,grainBufferCapacitySamples); grainBuffer.clear();
    grainWritePos=0; grainArmed=false;
    pendingTaps.clear(); activeTaps.clear();
    samplePosition=0;
}

// FIX (Phase 1): Forward/Reverse re-expressed as RepeatEvents instead of a bare position array - same
// exact positions PD already had (ground-truth values from the reference Cubase project's MIDI, see
// earlier session), volumeDb=0 for every event (the old volume curve's factory default was already
// flat, so this is not a behaviour change, just the same flat shape now living on each event instead
// of a separate curve object). "Magic statics" (a static local with a real initializer) guarantees
// this builds exactly once, thread-safe, with no manual flag/lock needed.
static PDAudioProcessor::RepeatPattern buildForwardLockedPreset(){
    PDAudioProcessor::RepeatPattern pat{};
    float p0[]={0.000f,0.007f,0.015f,0.024f,0.034f,0.047f,0.061f,0.078f,0.098f,0.121f,0.147f,0.178f,
                0.214f,0.256f,0.305f,0.362f,0.429f,
                0.4875f,0.5630f,0.6490f,0.7464f,0.8578f,0.9828f};
    pat.count=(int)(sizeof(p0)/sizeof(p0[0]));
    for(int i=0;i<pat.count;++i){
        auto& e=pat.events[i];
        e.id=i; e.position=p0[i]; e.volumeDb=0.f; e.pan=0.f;
        e.pitchSemitones=0.f; e.formantSemitones=0.f; e.reverse=false; e.enabled=true;
    }
    return pat;
}
// Reverse: the exact mirror of Forward (reflected around the centre of the measure, order reversed),
// same as before - computed FROM Forward so the two never silently drift apart.
static PDAudioProcessor::RepeatPattern buildReverseLockedPreset(){
    const auto fwd=buildForwardLockedPreset();
    PDAudioProcessor::RepeatPattern pat{};
    pat.count=fwd.count;
    for(int i=0;i<fwd.count;++i){
        auto& e=pat.events[i];
        e=fwd.events[fwd.count-1-i];
        e.id=i;
        e.position=juce::jmin(1.0f-fwd.events[fwd.count-1-i].position,1.0f)*0.985f;
    }
    return pat;
}
const PDAudioProcessor::RepeatPattern& PDAudioProcessor::getLockedPreset(int index){
    static const RepeatPattern forward = buildForwardLockedPreset();
    static const RepeatPattern reverse = buildReverseLockedPreset();
    return (juce::jlimit(0,1,index)==0) ? forward : reverse;
}

// ---- Custom pattern: lock-free double-buffer exchange (spec section 23) ----
PDAudioProcessor::RepeatPattern PDAudioProcessor::getCustomPatternForEditing() const {
    return customBuffers[(size_t)customActiveBuffer.load()]; // small POD - cheap to copy on the UI thread
}
void PDAudioProcessor::commitCustomPattern(const RepeatPattern& newPattern){
    int activeIdx=customActiveBuffer.load();
    int writeIdx=1-activeIdx;
    customBuffers[(size_t)writeIdx]=newPattern; // fully write the INACTIVE buffer first...
    customActiveBuffer.store(writeIdx);         // ...then atomically publish it. The audio thread
    // never sees a half-written pattern: it only ever reads customBuffers[customActiveBuffer.load()],
    // and that index only changes to a buffer that is already completely written.
}
const PDAudioProcessor::RepeatPattern& PDAudioProcessor::getActivePatternForAudio(int presetIndex) const {
    if(presetIndex==kPresetCustom) return customBuffers[(size_t)customActiveBuffer.load()];
    return getLockedPreset(presetIndex==kPresetReverse ? 1 : 0);
}

void PDAudioProcessor::scheduleTapsForTrigger(juce::int64 triggerSample){
    pendingTaps.clear();
    // Don't hard-clear active taps - snapshot each one's next few milliseconds of audio out of the
    // CURRENT grain buffer (about to be reused for this new note) into its own private tail, and
    // switch it into a forced linear fade-to-zero, so an interrupted tap fades quickly instead of
    // clicking.
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

    // FIX (Phase 1 - unified data model): gain now comes from each RepeatEvent's own volumeDb field
    // instead of a separate volume-curve lookup - the "one real data object per repeat" the spec
    // requires. Events are sorted by position before scheduling (the editor doesn't guarantee storage
    // order), and disabled events are skipped entirely.
    struct Sortable { float position; float gain; };
    std::vector<Sortable> sorted;
    sorted.reserve((size_t)pat.count);
    for(int i=0;i<pat.count;++i){
        const auto& e=pat.events[i];
        if(!e.enabled) continue;
        float gain=juce::Decibels::decibelsToGain(juce::jlimit(-12.f,12.f,e.volumeDb));
        sorted.push_back({juce::jlimit(0.f,1.f,e.position),gain});
    }
    std::sort(sorted.begin(),sorted.end(),[](const Sortable&a,const Sortable&b){return a.position<b.position;});

    for(auto& s:sorted){
        juce::int64 off=(juce::int64)std::round((double)s.position*(double)measureSamples);
        pendingTaps.push_back({triggerSample+off,s.gain,triggerSample,0});
    }
    // Each tap's playback is capped by the gap to the NEXT scheduled tap (not just the Grain Length
    // parameter), so closely-spaced repeats never overlap-blur into a smeared burst.
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
    if(ch<2){ samplePosition+=n; return; } // mono-in not supported by this effect's stereo grain design

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
                if(len>0) activeTaps.push_back({0,len,len,pendingTaps[t].gain});
                pendingTaps.erase(pendingTaps.begin()+(long)t);
            } else ++t;
        }

        float outL=L, outR=R; // dry, always untouched up to this point

        for(size_t t=0;t<activeTaps.size();){
            auto& tap=activeTaps[t];
            if(tap.forced){
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

// FIX (Phase 1 - spec section 24, DAW state): the Custom pattern must survive project save/reload.
// Serialized as a simple length-prefixed block of RepeatEvents, base64'd into the same APVTS
// ValueTree the rest of the state already uses - no separate save mechanism to keep in sync.
void PDAudioProcessor::getStateInformation(juce::MemoryBlock& destData){
    auto state=apvts.copyState();
    auto custom=getCustomPatternForEditing();
    juce::MemoryBlock patternBlock;
    patternBlock.append(&custom.count,sizeof(int));
    patternBlock.append(custom.events,sizeof(RepeatEvent)*kMaxRepeats);
    state.setProperty("customPatternData",patternBlock.toBase64Encoding(),nullptr);
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml,destData);
}
void PDAudioProcessor::setStateInformation(const void* data,int sizeInBytes){
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data,sizeInBytes));
    if(!xml || !xml->hasTagName(apvts.state.getType())) return;
    auto tree=juce::ValueTree::fromXml(*xml);
    apvts.replaceState(tree);

    RepeatPattern loaded{};
    const size_t expected=sizeof(int)+sizeof(RepeatEvent)*kMaxRepeats;
    juce::MemoryBlock mb;
    bool ok = tree.hasProperty("customPatternData")
        && mb.fromBase64Encoding(tree["customPatternData"].toString())
        && mb.getSize()>=expected;
    if(ok){
        int count=0; std::memcpy(&count,mb.getData(),sizeof(int));
        loaded.count=juce::jlimit(0,kMaxRepeats,count);
        std::memcpy(loaded.events,(const char*)mb.getData()+sizeof(int),sizeof(RepeatEvent)*kMaxRepeats);
    }
    // else: no saved custom pattern (older project, or nothing built yet) - loaded stays at count=0,
    // a valid empty pattern (spec section 15's "Empty Pattern" state), not an error case.
    commitCustomPattern(loaded);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(){ return new PDAudioProcessor(); }
