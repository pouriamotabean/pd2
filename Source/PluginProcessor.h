#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>

// PD - Decelerating Modulator
// Applies a periodic modulation (amplitude/filter/pitch) whose RATE starts fast and smoothly
// decelerates to a slower rate over a set time, re-triggered whenever a new transient is detected
// in the input signal (e.g. a single note on a track). Tempo-synced by default.
class PDAudioProcessor : public juce::AudioProcessor {
public:
    PDAudioProcessor();
    ~PDAudioProcessor() override = default;

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
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    enum class DecelCurve { Exponential, Linear, Logarithmic };
    enum class Target { Amplitude, Filter, Pitch };
    enum class Shape { Sine, Triangle, Square, Saw };
    // Tempo-synced note divisions for Start/End Rate, matching the reference UI's "1/16, 1/8..." style.
    static constexpr int kNumDivisions = 7;
    static constexpr const char* kDivisionNames[kNumDivisions] = {"1/32","1/16","1/8","1/4","1/2","1/1","2/1"};
    static constexpr float kDivisionBeats[kNumDivisions] = {0.125f,0.25f,0.5f,1.f,2.f,4.f,8.f};

    // ---- Parameters (plain atomics, matching PQ's style - no APVTS) ----
    std::atomic<bool> bypassed{false};
    std::atomic<bool> bpmSync{true};
    std::atomic<int> startDivIndex{1};      // default 1/16
    std::atomic<int> endDivIndex{3};        // default 1/4
    std::atomic<float> startRateHz{8.f};    // used when bpmSync == false
    std::atomic<float> endRateHz{2.f};
    std::atomic<float> decelTimeSec{2.0f};
    std::atomic<DecelCurve> decelCurve{DecelCurve::Exponential};
    std::atomic<float> depth{0.7f};
    std::atomic<Target> target{Target::Amplitude};
    std::atomic<Shape> shape{Shape::Sine};

    // ---- Read-only state for the UI ----
    std::atomic<float> uiCurrentRateHz{0.f};
    std::atomic<float> uiElapsedSinceTrigger{999.f}; // seconds since last (re)trigger, for the "x.xx s ago" readout
    std::atomic<bool> uiJustTriggered{false};        // pulses true briefly right after a retrigger, for the indicator dot
    double currentBpm=120.0;

    static float shapeCurve(float t, DecelCurve c);       // 0..1 -> 0..1, the deceleration envelope shape
    static float lfoWave(float phase01, Shape s);          // 0..1 phase -> -1..1 bipolar

private:
    double sr=44100.0;
    // Transient/onset detector: simple fast/slow envelope-follower pair (attack/release smoothing);
    // an onset is declared whenever the fast envelope rises meaningfully above the slow one, i.e. the
    // signal jumped up faster than the slow follower could track - classic simple onset detection.
    float envFast=0.f, envSlow=0.f;
    double timeSinceLastTrigger=0.0;
    static constexpr double kMinRetriggerGapSec=0.05; // debounce - ignore re-triggers closer together than this

    double phase=0.0;      // 0..1, the modulator's own running phase
    double elapsed=999.0;  // seconds since the last trigger - drives the deceleration curve

    // Filter target: a simple one-pole lowpass, cutoff swept by the modulator.
    float filterStateL=0.f, filterStateR=0.f;
    // Pitch target: a short modulated delay line (vibrato/chorus technique).
    static constexpr int kPitchDelayBufSize=4096; // enough for ~90ms at 44.1kHz, plenty of headroom
    std::array<float,kPitchDelayBufSize> pitchDelayL{}, pitchDelayR{};
    int pitchWriteIdx=0;

    float currentRateFor(double elapsedSec) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessor)
};
