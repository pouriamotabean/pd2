#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>

// PD - Decelerating Modulator
// Core DSP behavior is intentionally preserved from v0.1:
// transient -> fast modulation -> smooth rate deceleration -> retrigger.
// v0.2 adds a proper APVTS parameter layer for DAW automation/state recall and
// makes the detector/effect stages more robust without changing the exposed ranges.
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

    static constexpr int kNumDivisions = 7;
    static constexpr const char* kDivisionNames[kNumDivisions] = {"1/32","1/16","1/8","1/4","1/2","1/1","2/1"};
    static constexpr float kDivisionBeats[kNumDivisions] = {0.125f,0.25f,0.5f,1.f,2.f,4.f,8.f};

    // Public mirrors retained for compatibility with the original UI/source.
    std::atomic<bool> bypassed{false};
    std::atomic<bool> bpmSync{true};
    std::atomic<int> startDivIndex{1};
    std::atomic<int> endDivIndex{3};
    std::atomic<float> startRateHz{8.f};
    std::atomic<float> endRateHz{2.f};
    std::atomic<float> decelTimeSec{2.0f};
    std::atomic<DecelCurve> decelCurve{DecelCurve::Exponential};
    std::atomic<float> depth{0.7f};
    std::atomic<Target> target{Target::Amplitude};
    std::atomic<Shape> shape{Shape::Sine};

    std::atomic<float> uiCurrentRateHz{0.f};
    std::atomic<float> uiElapsedSinceTrigger{999.f};
    std::atomic<bool> uiJustTriggered{false};
    std::atomic<float> uiInputLevelDb{-100.f};
    std::atomic<int> uiTriggerCount{0};
    double currentBpm = 120.0;

    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static float shapeCurve(float t, DecelCurve c);
    static float lfoWave(float phase01, Shape s);

private:
    double sr = 44100.0;

    // More stable onset detector: fast/slow followers + adaptive floor + hysteresis.
    float envFast = 0.f, envSlow = 0.f;
    float detectorFloor = 0.f;
    bool detectorArmed = true;
    double timeSinceLastTrigger = 0.0;
    static constexpr double kMinRetriggerGapSec = 0.05;

    double phase = 0.0;
    double elapsed = 999.0;

    float filterStateL = 0.f, filterStateR = 0.f;
    float filterCoeff = 0.f;

    static constexpr int kPitchDelayBufSize = 4096;
    std::array<float,kPitchDelayBufSize> pitchDelayL{}, pitchDelayR{};
    int pitchWriteIdx = 0;

    void syncAtomicsFromParameters();
    float currentRateFor(double elapsedSec) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PDAudioProcessor)
};
