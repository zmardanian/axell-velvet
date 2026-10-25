#pragma once
#include <axell/gui/LookAndFeel.h>
#include <array>
#include "PluginProcessor.h"

namespace vui
{
constexpr int kWidth = 1120;
constexpr int kHeight = 700;
}

// scrolling history (~4 s): the 2-17 kHz level against the voice body (coloured by the detector's confidence), the
// AMOUNT target, and the de-esser's reduction hanging from the top
class SibilanceView : public juce::Component, public juce::SettableTooltipClient
{
public:
    SibilanceView();
    void push (const axv::DisplayFrame& f);
    void paint (juce::Graphics&) override;
    void setActive (bool a) { active = a; }

private:
    std::vector<axv::DisplayFrame> ring;
    int pos = 0;
    bool active = true;
};

// the HEAT model's transfer at the live operating level: a 100 Hz sine at the recent input peak through a copy of the
// model (tape shows its hysteresis loop), its harmonics, and S-GUARD at work
class HeatView : public juce::Component, public juce::SettableTooltipClient
{
public:
    void update (const axv::Params& p, float inPeakDb, float guardWeight);
    void paint (juce::Graphics&) override;

private:
    void compute();
    axv::Params params;
    float peakDb = -120.0f, peakHold = -120.0f, guardShown = 1.0f, lastAmp = -1.0f;
    int lastMode = -1, hold = 0;
    std::vector<juce::Point<float>> loop;
    std::array<float, 9> harm {}; // H2..H10 in dBc at the live level
};

class VelvetEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VelvetEditor (VelvetProcessor&);
    ~VelvetEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshNow() { timerCallback(); }

private:
    class Content : public juce::Component
    {
    public:
        explicit Content (VelvetEditor& e) : owner (e) {}
        void paint (juce::Graphics&) override;
        void resized() override;
        VelvetEditor& owner;
    };
    void timerCallback() override;
    void layout();
    void showPresetMenu();
    void promptSavePreset();

    VelvetProcessor& proc;
    ui::LookAndFeel lnf;
    Content content { *this };
    juce::TooltipWindow tooltips { this, 700 };

    juce::TextButton presetPrev { "<" }, presetNext { ">" }, presetSave { "SAVE" };
    ui::MenuButton presetName;
    juce::TextButton slotA { "A" }, slotB { "B" }, slotCopy { "COPY" };

    ui::Toggle deessOn { "", ui::col::teal, "power" }, heatOn { "", ui::col::accent, "power" };
    SibilanceView sibView;
    ui::Knob amount { "AMOUNT", ui::col::teal, 132 }, range { "RANGE", ui::col::teal, 62 };
    ui::Segmented focus { { "AUTO", "S", "SH" }, ui::col::teal };
    ui::Segmented deessMode { { "STUDIO", "LIVE" }, ui::col::teal };
    ui::Toggle listen { "LISTEN", ui::col::warn, "pill" };

    ui::Segmented mode { { "TAPE", "CONSOLE", "TUBE" }, ui::col::accent };
    HeatView heatView;
    ui::Knob drive { "DRIVE", ui::col::accent, 132 }, guard { "S-GUARD", ui::col::accent, 62 }, tone { "TONE", ui::col::accent, 62 },
        mix { "MIX", ui::col::accent, 62 };

    ui::Knob level { "LEVEL", ui::col::global, 62 }, output { "OUTPUT", ui::col::global, 62 };
    juce::TextButton learn { "LEARN" };
    ui::Segmented os { { "2x", "4x", "8x", "16x" }, ui::col::global };
    ui::Segmented lowCut { { "OFF", "30", "60", "90" }, ui::col::global };
    ui::LevelMeter inMeter { "IN" }, outMeter { "OUT" };

    juce::Rectangle<int> headerArea, deessArea, heatArea, bottomArea, focusLabel, modeLabel, osLabel, lowCutLabel, learnText;
    juce::String learnStatus;
    float grNow = 0.0f;
    std::unique_ptr<juce::AlertWindow> saveDialog;
    bool constructed = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VelvetEditor)
};
