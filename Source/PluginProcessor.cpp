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

// FIX (Phase 3): adapted from the same phase-vocoder design (and bug fixes) used in PV - correct
// same-modulus overlap-add, phase accumulation via true-frequency estimation. See the header comment
// on PitchVocoderEngine for why the window is 512 (not the more typical 2048) for this use case.
void PDAudioProcessor::PitchVocoderEngine::prepare(){
    inRing.assign((size_t)N,0.f); outRing.assign((size_t)N,0.f);
    window.resize((size_t)N); prevPhase.assign((size_t)(N/2+1),0.0); sumPhase.assign((size_t)(N/2+1),0.0);
    fftIn.resize((size_t)(2*N)); fftOut.resize((size_t)(2*N));
    magBuf.assign((size_t)(N/2+1),0.f); envBuf.assign((size_t)(N/2+1),0.f); excitBuf.assign((size_t)(N/2+1),0.f);
    fft=std::make_unique<juce::dsp::FFT>((int)std::log2(N));
    for(int i=0;i<N;++i) window[(size_t)i]=0.5f-0.5f*std::cos(juce::MathConstants<float>::twoPi*i/(N-1));
    pos=0;
}
void PDAudioProcessor::PitchVocoderEngine::reset(double newPitchRatio, double newFormantRatio){
    pitchRatio=newPitchRatio; formantRatio=newFormantRatio;
    std::fill(inRing.begin(),inRing.end(),0.f); std::fill(outRing.begin(),outRing.end(),0.f);
    std::fill(prevPhase.begin(),prevPhase.end(),0.0); std::fill(sumPhase.begin(),sumPhase.end(),0.0);
    pos=0;
}
float PDAudioProcessor::PitchVocoderEngine::process(float x){
    inRing[(size_t)pos]=x;
    const int read=pos;
    float y=outRing[(size_t)read];
    outRing[(size_t)read]=0.f;

    if(pos % H==0){
        for(int i=0;i<N;++i){
            int idx=(pos-i+N)%N;
            fftIn[(size_t)(2*i)]=inRing[(size_t)idx]*window[(size_t)i];
            fftIn[(size_t)(2*i+1)]=0.f;
        }
        fft->performRealOnlyForwardTransform(fftIn.data());

        // Pass 1: magnitude per bin (phase is read straight from fftIn again in pass 2 - untouched
        // by anything formant-related, since formant must never affect WHERE energy ends up, only
        // how loud each frequency band is).
        for(int k=0;k<=N/2;++k){
            const double re=fftIn[(size_t)(2*k)], im=fftIn[(size_t)(2*k+1)];
            magBuf[(size_t)k]=(float)std::sqrt(re*re+im*im);
        }

        // FIX (Phase 4): formant envelope extraction + warp - skipped entirely (zero extra cost) when
        // formant is centred, so Phase 3's pure pitch-shifting is completely unaffected.
        if(std::abs(formantRatio-1.0)>0.001){
            const int halfWin=juce::jmax(1,N/64); // smoothing radius in bins - a broad, formant-scale window
            for(int k=0;k<=N/2;++k){
                double sum=0.0; int count=0;
                for(int j=juce::jmax(0,k-halfWin); j<=juce::jmin(N/2,k+halfWin); ++j){ sum+=magBuf[(size_t)j]; ++count; }
                envBuf[(size_t)k]=(float)(sum/juce::jmax(1,count));
            }
            for(int k=0;k<=N/2;++k) excitBuf[(size_t)k]=magBuf[(size_t)k]/(envBuf[(size_t)k]+1e-6f);
            for(int k=0;k<=N/2;++k){
                double srcK=(double)k/formantRatio;
                int k0=juce::jlimit(0,N/2,(int)std::floor(srcK));
                int k1=juce::jlimit(0,N/2,k0+1);
                float frac=(float)(srcK-k0);
                float warpedEnv=envBuf[(size_t)k0]*(1.f-frac)+envBuf[(size_t)k1]*frac;
                magBuf[(size_t)k]=excitBuf[(size_t)k]*warpedEnv; // overwrite with the formant-shifted magnitude
            }
        }

        std::fill(fftOut.begin(), fftOut.end(), 0.0f);
        const double expected = juce::MathConstants<double>::twoPi * H / N;

        for (int k=0;k<=N/2;++k)
        {
            const double re=fftIn[(size_t)(2*k)], im=fftIn[(size_t)(2*k+1)];
            const double mag=magBuf[(size_t)k]; // possibly formant-warped above - phase still from the original analysis
            double phase=std::atan2(im,re);
            double delta=phase-prevPhase[(size_t)k]-expected*k;
            delta=juce::jlimit(-juce::MathConstants<double>::pi,
                               juce::MathConstants<double>::pi, delta);
            const double trueFreq=juce::MathConstants<double>::twoPi*k/N + delta/H;
            const double newBin=k*pitchRatio;
            if (newBin <= N/2-2)
            {
                const int b=(int)std::floor(newBin);
                const double frac=newBin-b;
                const double targetPhase = sumPhase[(size_t)k] + trueFreq*H*pitchRatio;
                sumPhase[(size_t)k]=targetPhase;
                prevPhase[(size_t)k]=phase;

                const float a=(float)(mag*std::cos(targetPhase));
                const float q=(float)(mag*std::sin(targetPhase));
                fftOut[(size_t)(2*b)] += a*(float)(1-frac); fftOut[(size_t)(2*b+1)] += q*(float)(1-frac);
                fftOut[(size_t)(2*(b+1))] += a*(float)frac; fftOut[(size_t)(2*(b+1)+1)] += q*(float)frac;
            }
        }

        fft->performRealOnlyInverseTransform(fftOut.data());
        const float norm = 1.0f / (float)(N * 0.5);
        for (int i=0;i<N;++i)
        {
            int idx=(pos+i)%N;
            outRing[(size_t)idx] += fftOut[(size_t)(2*i)]*window[(size_t)i]*norm;
        }
    }

    pos=(pos+1)%N;
    return y;
}
int PDAudioProcessor::checkoutPitchEngine(double pitchRatio, double formantRatio){
    for(int i=0;i<kPitchPoolSize;++i){
        if(!pitchPool[(size_t)i].inUse){ pitchPool[(size_t)i].inUse=true; pitchPool[(size_t)i].reset(pitchRatio,formantRatio); return i; }
    }
    return -1; // pool exhausted - caller falls back to unshifted playback for this tap rather than blocking/crashing
}
void PDAudioProcessor::releasePitchEngine(int slot){
    if(slot>=0 && slot<kPitchPoolSize) pitchPool[(size_t)slot].inUse=false;
}

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
    for(auto& eng:pitchPool){ eng.inUse=false; eng.prepare(); }
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
            int srcPos = tap.reverse ? (tap.totalSamples-1-(tap.grainReadPos+k)) : (tap.grainReadPos+k);
            bool valid = srcPos>=0 && srcPos<grainWritePos;
            tap.tailL[(size_t)k]= valid ? grainBuffer.getSample(0,srcPos) : 0.f;
            tap.tailR[(size_t)k]= valid ? grainBuffer.getSample(1,srcPos) : 0.f;
        }
        tap.grainReadPos=0; tap.samplesRemaining=tap.forcedTotal; tap.totalSamples=tap.forcedTotal;
    }

    int presetIdx=(int)apvts.getRawParameterValue(pPreset)->load();
    const auto& pat=getActivePatternForAudio(presetIdx);

    double bpm=uiBpm.load(); int num=uiTimeSigNumerator.load();
    double measureSec = (60.0/juce::jmax(1.0,bpm)) * juce::jmax(1,num);
    juce::int64 measureSamples=(juce::int64)std::round(measureSec*sr);

    struct Sortable { float position; float gain; float pan; float pitchSemitones; float formantSemitones; bool reverse; };
    std::vector<Sortable> sorted;
    sorted.reserve((size_t)pat.count);
    for(int i=0;i<pat.count;++i){
        const auto& e=pat.events[i];
        if(!e.enabled) continue;
        float gain=juce::Decibels::decibelsToGain(juce::jlimit(-12.f,12.f,e.volumeDb));
        sorted.push_back({juce::jlimit(0.f,1.f,e.position),gain,juce::jlimit(-1.f,1.f,e.pan),
                           juce::jlimit(-12.f,12.f,e.pitchSemitones),juce::jlimit(-12.f,12.f,e.formantSemitones),e.reverse});
    }
    std::sort(sorted.begin(),sorted.end(),[](const Sortable&a,const Sortable&b){return a.position<b.position;});

    for(auto& s:sorted){
        juce::int64 off=(juce::int64)std::round((double)s.position*(double)measureSamples);
        pendingTaps.push_back({triggerSample+off,s.gain,s.pan,s.pitchSemitones,s.formantSemitones,s.reverse,triggerSample,0});
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
                if(len>0){
                    ActiveTap newTap;
                    newTap.grainReadPos=0; newTap.samplesRemaining=len; newTap.totalSamples=len;
                    newTap.gain=pendingTaps[t].gain; newTap.pan=pendingTaps[t].pan; newTap.reverse=pendingTaps[t].reverse;
                    // FIX (Phase 3/4): only touch the pool for taps that actually need shifting - the
                    // common case (pitch==0 AND formant==0) plays the raw grain exactly as every phase
                    // before this one did, byte-for-byte, at zero extra CPU cost.
                    if(std::abs(pendingTaps[t].pitchSemitones)>0.01f || std::abs(pendingTaps[t].formantSemitones)>0.01f){
                        double pitchRatio=std::pow(2.0,(double)pendingTaps[t].pitchSemitones/12.0);
                        double formantRatio=std::pow(2.0,(double)pendingTaps[t].formantSemitones/12.0);
                        newTap.pvL=checkoutPitchEngine(pitchRatio,formantRatio);
                        newTap.pvR=checkoutPitchEngine(pitchRatio,formantRatio);
                        if(newTap.pvL<0 || newTap.pvR<0){
                            // Pool exhausted (very heavy simultaneous pitch/formant-shifted repeats) -
                            // release whichever slot WAS granted and fall back to unshifted playback
                            // for this one tap, rather than blocking or crashing.
                            releasePitchEngine(newTap.pvL); releasePitchEngine(newTap.pvR);
                            newTap.pvL=-1; newTap.pvR=-1;
                        } else {
                            // FIX (real bug found in review - very likely why Pitch/Formant produced no
                            // audible effect): a freshly-reset vocoder's ring buffer is all zero, so its
                            // FIRST analysis window is mostly silence - for a tap shorter than or close
                            // to N (512 samples, ~11.6ms), the shifted output can be negligible or never
                            // arrive at all before the tap ends. Since this tap's own audio is already
                            // fully captured (that's what "len" IS), we can pre-feed the engine with it
                            // once, discarding the (meaningless, cold-start) output, so its ring buffer
                            // already holds real audio before real playback starts from position 0.
                            int primeCount=juce::jmin(PitchVocoderEngine::N,len);
                            for(int k=0;k<primeCount;++k){
                                int srcIdx = newTap.reverse ? (len-1-k) : k;
                                if(srcIdx<0 || srcIdx>=grainWritePos) break; // never read uncaptured/stale data
                                pitchPool[(size_t)newTap.pvL].process(grainBuffer.getSample(0,srcIdx));
                                pitchPool[(size_t)newTap.pvR].process(grainBuffer.getSample(1,srcIdx));
                            }
                        }
                    }
                    activeTaps.push_back(newTap);
                }
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
                if(tap.samplesRemaining<=0){ releasePitchEngine(tap.pvL); releasePitchEngine(tap.pvR); activeTaps.erase(activeTaps.begin()+(long)t); } else ++t;
                continue;
            }
            // FIX (Phase 5): a reversed tap reads the SAME captured window [0, totalSamples) as a
            // forward tap, just back-to-front - at playback-progress 0 it reads the most recently
            // captured sample in its window, ending on the sample closest to the trigger. Since that
            // whole window was captured before this tap even started reading (see the "next tap
            // distance" cap and the trigger-time availability check that fixed totalSamples in the
            // first place), the reversed index is always safely behind grainWritePos too - no separate
            // synchronization case needed.
            int readIdx = tap.reverse ? (tap.totalSamples-1-tap.grainReadPos) : tap.grainReadPos;
            if(readIdx>=0 && readIdx<grainWritePos && tap.samplesRemaining>0){
                float fade=1.f;
                int fadeLen=juce::jmin(kFadeSamples,tap.totalSamples/2);
                int posIntoTap=tap.totalSamples-tap.samplesRemaining;
                if(fadeLen>0){
                    if(posIntoTap<fadeLen) fade=0.5f-0.5f*std::cos(juce::MathConstants<float>::pi*(float)posIntoTap/(float)fadeLen);
                    else if(tap.samplesRemaining<=fadeLen) fade=0.5f-0.5f*std::cos(juce::MathConstants<float>::pi*(float)tap.samplesRemaining/(float)fadeLen);
                }
                float gL=grainBuffer.getSample(0,readIdx), gR=grainBuffer.getSample(1,readIdx);
                if(tap.pvL>=0) gL=pitchPool[(size_t)tap.pvL].process(gL);
                if(tap.pvR>=0) gR=pitchPool[(size_t)tap.pvR].process(gR);
                outL += gL*tap.gain*fade*panGainL; outR += gR*tap.gain*fade*panGainR;
                ++tap.grainReadPos; --tap.samplesRemaining;
            }
            if(tap.samplesRemaining<=0){ releasePitchEngine(tap.pvL); releasePitchEngine(tap.pvR); activeTaps.erase(activeTaps.begin()+(long)t); } else ++t;
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
