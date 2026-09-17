#pragma once
#include <JuceHeader.h>
#include <array>

// PD - "Repeat Pattern" - complete redesign per the reference-audio analysis session:
// the effect is NOT a continuously-decelerating LFO. It's a fixed rhythmic TAP PATTERN (positions
// expressed as a fraction of one measure, so it scales to any tempo) where each tap replays a short
// captured grain of the input's own attack, at a volume taken from an editable VOLUME CURVE (also
// one measure long). The original input is never touched - it passes through 100% dry, untouched,
// exactly as recorded, so the attack/punch of the source note is never altered. The taps are added
// ON TOP of that dry signal, not instead of it.
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

    // ---- Parameters (APVTS - Bypass is host-automatable; pattern/volume-curve choice are simple
    // discrete selections, also parameters so a saved project remembers the choice) ----
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static constexpr const char* pBypass = "bypass";
    static constexpr const char* pPattern = "pattern";
    static constexpr const char* pVolumeCurve = "volumeCurve";
    static constexpr const char* pGrainMs = "grainMs";

    // ---- UI-readable live state (written by the audio thread, read by the editor's timer) ----
    std::atomic<float> uiElapsedSinceTrigger{999.f};
    std::atomic<int> uiTriggerCount{0};
    std::atomic<double> uiBpm{120.0};
    std::atomic<int> uiTimeSigNumerator{4};

    // ---- Pattern data: onset positions as a FRACTION OF ONE MEASURE (0..1) - tempo AND time-
    // signature independent by construction, since "one measure" is whatever the host says it is.
    static constexpr int kMaxPatternPoints = 40;
    struct Pattern { int count=0; float positions[kMaxPatternPoints]{}; };
    static const Pattern& getPattern(int index); // index 0 = the only populated one for now

    // ---- Volume curve: a handful of (x = fraction of measure, y = gain multiplier) control points,
    // smoothly interpolated (smootherstep between segments) - drag-editable in the UI (y only, for
    // now; x positions are fixed anchors, matching "3 preset shapes" rather than free-form points).
    // FIX (found while investigating "doesn't reset to flat"): this used to be process-global static
    // data - a) shared across every open instance of the plugin (editing the curve in one instance
    // silently changed every other open instance too), and b) never saved or loaded with the project
    // at all, so a reload wasn't restoring the "flat" default - it was just showing whatever the last
    // edit anywhere in this DAW session left behind. It's real per-instance state now, and is written
    // to/read from getStateInformation()/setStateInformation() below.
    struct VolumePoint { float x, y; };
    static constexpr int kMaxVolumePoints = 6;
    struct VolumeCurve { int count=0; VolumePoint points[kMaxVolumePoints]{}; };
    static VolumeCurve defaultVolumeCurve(int index); // the flat-start factory default for each slot
    static float evalVolumeCurve(const VolumeCurve&, float x);
    // FIX (real bug found in review): volumeCurves used to be a plain public array, written directly
    // by the UI thread's mouseDrag (with the person actively dragging DURING playback - literally the
    // main use case) while the audio thread read it, unguarded, in scheduleTapsForTrigger(). That's a
    // genuine data race - a torn read could hand the audio thread a point with an old x paired with a
    // half-written new y. It's private now, reachable only through these two lock-guarded methods.
    VolumeCurve getVolumeCurveSnapshot(int curveIdx) const; // safe to call from either thread
    void setVolumeCurvePointY(int curveIdx, int pointIdx, float y); // UI thread only, but safe regardless

private:
    VolumeCurve volumeCurves[3]; // per-instance, editable, saved with the project - see accessors above
    mutable juce::SpinLock volumeCurvesLock; // held only across the tiny copy/write below, never per-sample
    double sr = 44100.0;

    // Onset (transient) detector - same fast/slow envelope-follower trigger used in the original PD.
    float envFast=0.f, envSlow=0.f;
    juce::int64 samplesSinceLastTrigger = 1LL<<40;

    // Grain capture: records the input starting the instant a trigger fires, so taps have real,
    // untouched source audio to replay - never a synthesized tone. One mono-summed detector envelope
    // decides WHEN to trigger; the grain itself is captured in full stereo.
    // FIX (found while reviewing before delivery): a fixed 96000-sample cap was smaller than ONE
    // MEASURE at 75 BPM already (3.2s = 141,120 samples at 44.1kHz) - late taps in the pattern would
    // have silently gone quiet once capture hit that ceiling. Sized in prepareToPlay() instead, from
    // the real sample rate, generously (8 seconds - comfortably covers a full measure even at very
    // slow tempos, e.g. 4/4 at 60 BPM is still only 4s).
    int grainBufferCapacitySamples = 96000; // placeholder; recomputed in prepareToPlay()
    juce::AudioBuffer<float> grainBuffer; // 2 x grainBufferCapacitySamples, re-armed fresh per trigger
    int grainWritePos = 0;
    bool grainArmed = false;

    struct PendingTap { juce::int64 startSample; float gain; juce::int64 triggerSample; juce::int64 maxLenSamples; };
    // FIX (requested #3 - click + volume drop on fast/overlapping notes): a new trigger used to call
    // activeTaps.clear() on whatever was still playing from the previous note, and clear the grain
    // buffer right after - so any tap's contribution to the output dropped from its current level
    // straight to zero on the very next sample. That instantaneous jump is an actual audio
    // discontinuity - the click. Every ActiveTap now carries a small private "tail" snapshot (copied
    // out of the grain buffer at the moment it's force-stopped, before that buffer gets reused for
    // the new note) and a forced linear ramp-to-zero, so an interrupted tap always fades out over a
    // few milliseconds instead of stopping dead.
    static constexpr int kForceFadeSamples = 256; // ~5.8ms @44.1kHz - short but click-free
    struct ActiveTap {
        int grainReadPos=0; int samplesRemaining=0; int totalSamples=0; float gain=1.f;
        bool forced=false; int forcedTotal=0; // forcedTotal = kForceFadeSamples at the moment of interruption
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
