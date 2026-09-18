#pragma once
#include <JuceHeader.h>
#include <array>

// PD - Phase 1 of the "Interactive Repeat Pattern Editor" rewrite (see the full spec doc: PD is
// moving from a fixed repeat pattern to a per-repeat programmable one). The old model - a bare array
// of positions plus a SEPARATE volume curve the UI edited independently - is replaced here with ONE
// real data object per repeat (RepeatEvent), shared by the UI, the preset system and the audio
// engine, exactly as the spec requires (section 12): "The UI must not maintain a separate fake
// representation." Phase 1 wires up position + volumeDb; pan/pitch/formant/reverse fields already
// exist on every event so later phases are additive, not a data-model migration.
class PDAudioProcessor : public juce::AudioProcessor {
public:
    PDAudioProcessor();
    ~PDAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PD"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // ---- Parameters ----
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static constexpr const char* pBypass = "bypass";
    // FIX (kept the original parameter ID string "pattern" for state/automation continuity even
    // though its meaning has grown): now a 3-way preset selector - Forward / Reverse (both locked,
    // "in their current form" per the explicit request) / Custom (the new fully-editable pattern).
    static constexpr const char* pPreset = "pattern";
    static constexpr const char* pGrainMs = "grainMs";
    enum PresetIndex { kPresetForward=0, kPresetReverse=1, kPresetCustom=2 };

    // ---- UI-readable live state (written by the audio thread, read by the editor's timer) ----
    std::atomic<float> uiElapsedSinceTrigger{999.f};
    std::atomic<int> uiTriggerCount{0};
    std::atomic<double> uiBpm{120.0};
    std::atomic<int> uiTimeSigNumerator{4};

    // ---- Unified data model (spec section 12) ----
    struct RepeatEvent {
        int id=0;
        float position=0.f;          // fraction of one measure, 0..1 - X axis, ALWAYS time (spec #3/#25)
        float volumeDb=0.f;          // -12..+12 - Phase 1 (VOLUME edit mode)
        float pan=0.f;                // -1(L)..+1(R) - wired in Phase 2 (PAN edit mode)
        float pitchSemitones=0.f;     // -12..+12 - wired in Phase 3 (Phase Vocoder engine)
        float formantSemitones=0.f;   // -12..+12 - wired in Phase 4
        bool reverse=false;           // wired in Phase 5
        bool enabled=true;
    };
    static constexpr int kMaxRepeats=64;
    struct RepeatPattern { int count=0; RepeatEvent events[kMaxRepeats]{}; };

    // FIX (requested): Forward and Reverse are no longer fully immutable. Their POSITIONS stay fixed
    // (that's a UI-level editing policy - the editor simply never lets a drag/create/delete gesture
    // touch position for these two presets - not a data-model restriction), but volumeDb (and, as
    // later phases land, pan/pitch/formant) are genuinely editable and saved with the project, exactly
    // like Custom. All three presets are therefore just three named, independently-editable
    // RepeatPatterns now - Forward/Reverse simply start from the tuned rhythmic shape instead of
    // starting empty, and the editor prevents repositioning/adding/removing repeats on them.
    //
    // Lock-free double-buffer exchange (spec section 23), one pair per preset - the UI writes a
    // COMPLETE new pattern into whichever buffer is currently INACTIVE for that preset, then flips an
    // atomic index; the audio thread only ever reads whichever buffer is currently active, and that
    // buffer is never mutated again until it becomes inactive - so there is no torn-read window and no
    // lock is ever taken on the audio thread.
    RepeatPattern getPatternForEditing(int presetIndex) const; // UI thread - a working copy to edit and hand back
    void commitPattern(int presetIndex, const RepeatPattern& newPattern); // UI thread - publishes an edited copy
    const RepeatPattern& getActivePatternForAudio(int presetIndex) const; // safe from either thread

private:
    // buffers[presetIndex][0 or 1]; active[presetIndex] says which of the pair is currently live.
    std::array<std::array<RepeatPattern,2>,3> presetBuffers{};
    std::array<std::atomic<int>,3> presetActiveBuffer{};
    static RepeatPattern buildForwardSeed(); // the tuned shape Forward/Reverse start from - see .cpp
    static RepeatPattern buildReverseSeed();

    double sr = 44100.0;

    // Onset (transient) detector - same fast/slow envelope-follower trigger used since the original PD.
    float envFast=0.f, envSlow=0.f;
    juce::int64 samplesSinceLastTrigger = 1LL<<40;

    // Grain capture: records the input starting the instant a trigger fires, so taps have real,
    // untouched source audio to replay - never a synthesized tone.
    int grainBufferCapacitySamples = 96000; // placeholder; recomputed in prepareToPlay() from sample rate
    juce::AudioBuffer<float> grainBuffer; // 2 x grainBufferCapacitySamples, re-armed fresh per trigger
    int grainWritePos = 0;
    bool grainArmed = false;

    struct PendingTap { juce::int64 startSample; float gain; float pan; juce::int64 triggerSample; juce::int64 maxLenSamples; };
    // A new trigger force-fades (not hard-clears) whatever was still playing from the previous note -
    // see scheduleTapsForTrigger()/processBlock() - avoiding an audible click on fast/overlapping notes.
    static constexpr int kForceFadeSamples = 256; // ~5.8ms @44.1kHz - short but click-free
    struct ActiveTap {
        int grainReadPos=0; int samplesRemaining=0; int totalSamples=0; float gain=1.f; float pan=0.f;
        bool forced=false; int forcedTotal=0;
        std::array<float,kForceFadeSamples> tailL{}, tailR{}; // only used once forced==true
    };
    std::vector<PendingTap> pendingTaps;
    std::vector<ActiveTap> activeTaps;
    juce::int64 samplePosition = 0; // running absolute sample counter, never reset

    int grainLengthSamplesFor(float desiredMs) const { return (int)std::round(desiredMs*0.001*sr); }
    static constexpr int kFadeSamples = 48; // short raised-cosine in/out on every tap's OWN natural start/end

    void scheduleTapsForTrigger(juce::int64 triggerSample);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessor)
};
