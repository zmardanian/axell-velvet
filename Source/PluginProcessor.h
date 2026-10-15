#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine.h"
#include "Presets.h"

// Thin JUCE wrapper around axv::Engine (all DSP lives in JUCE-free headers).
class VelvetProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    VelvetProcessor();
    ~VelvetProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    void setNonRealtime (bool isNonRealtime) noexcept override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    //== presets / A-B / LEARN (message thread) ================================================
    void applyPreset (const axv::FactoryPreset& p);
    void applyParams (const axv::Params& p, bool includeLevel);
    axv::Params readParams() const;
    static juce::File userPresetFolder();
    bool saveUserPreset (const juce::String& name);
    bool loadUserPreset (const juce::File& file);
    juce::String getPresetName() const;
    void setPresetName (const juce::String& n);
    int getActiveSlot() const { return activeSlot; }
    void selectSlot (int slot);
    void copyActiveToOther();
    // LEARN: restart the input loudness measurement; after some playback, applyLearn() sets LEVEL so the vocal sits at
    // the nominal -18 LUFS operating level
    void startLearn();
    bool isLearning() const { return learning; }
    float learnSuggestionDb() const; // NaN until enough audio was measured
    void applyLearn();

    axv::Engine& engine() { return dsp; }

private:
    void timerCallback() override;
    void syncEngineParams (bool bypass);
    void restoreTree (const juce::ValueTree& tree);

    axv::Engine dsp;
    int preparedChannels = 0;
    std::atomic<int> currentProgram { 0 };
    std::atomic<bool> programNamePending { false };
    juce::CriticalSection nameLock;
    juce::String presetName { "Init" };
    int activeSlot = 0;
    juce::ValueTree slots[2];
    bool learning = false;

    std::atomic<float>* pDeessOn = nullptr; std::atomic<float>* pAmount = nullptr; std::atomic<float>* pRange = nullptr;
    std::atomic<float>* pFocus = nullptr;   std::atomic<float>* pListen = nullptr; std::atomic<float>* pHeatOn = nullptr;
    std::atomic<float>* pMode = nullptr;    std::atomic<float>* pDrive = nullptr;  std::atomic<float>* pGuard = nullptr;
    std::atomic<float>* pTone = nullptr;    std::atomic<float>* pMix = nullptr;    std::atomic<float>* pLevel = nullptr;
    std::atomic<float>* pOutput = nullptr;  std::atomic<float>* pOs = nullptr;    std::atomic<float>* pLowCut = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VelvetProcessor)
};
