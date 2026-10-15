#include "PluginProcessor.h"
#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;

namespace
{
juce::AudioParameterFloatAttributes fmt (std::function<juce::String (float)> toText,
                                         std::function<float (const juce::String&)> fromText = nullptr)
{
    auto a = juce::AudioParameterFloatAttributes().withStringFromValueFunction ([toText] (float v, int) { return toText (v); });
    if (fromText == nullptr)
        fromText = [] (const juce::String& t) { return t.getFloatValue(); };
    return a.withValueFromStringFunction (fromText);
}
juce::String dbText (float v) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String pctText (float v) { return juce::String (juce::roundToInt (v)) + " %"; }
const juce::Identifier kPresetNameId ("presetName");
} // namespace

APVTS::ParameterLayout VelvetProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    // IDs, ranges and order are frozen from 1.0 on (saved projects, automation); new parameters are appended
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "deessOn", 1 }, "De-Ess On", true));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "De-Ess Amount",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, fmt ([] (float v) { return v < 0.05f ? String ("Off") : pctText (v); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "range", 1 }, "De-Ess Range",
        NormalisableRange<float> (3.0f, 30.0f, 0.1f), 18.0f, fmt ([] (float v) { return String (v, 1) + " dB"; })));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "focus", 1 }, "De-Ess Focus", StringArray { "Auto", "S", "SH" }, 0));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "listen", 1 }, "De-Ess Listen", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "heatOn", 1 }, "Heat On", true));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Heat Mode", StringArray { "Tape", "Console", "Tube" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "drive", 1 }, "Drive",
        NormalisableRange<float> (0.0f, 10.0f, 0.01f), 4.0f, fmt ([] (float v) { return String (v, 1); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "guard", 1 }, "S-Guard",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f, fmt (pctText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tone",
        NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f,
        fmt ([] (float v) { return std::abs (v) < 0.5f ? String ("Neutral") : (v < 0 ? "Dark " : "Bright ") + String (juce::roundToInt (std::abs (v))); })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Heat Mix",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f, fmt (pctText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "level", 1 }, "Level",
        NormalisableRange<float> (-24.0f, 36.0f, 0.1f), 0.0f, fmt (dbText)));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f, fmt (dbText)));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "os", 1 }, "Oversampling", StringArray { "2x", "4x", "8x", "16x" }, 3));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "lowcut", 1 }, "Low Cut", StringArray { "Off", "30 Hz", "60 Hz", "90 Hz" }, 1));
    return { p.begin(), p.end() };
}

VelvetProcessor::VelvetProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pDeessOn = apvts.getRawParameterValue ("deessOn");
    pAmount = apvts.getRawParameterValue ("amount");
    pRange = apvts.getRawParameterValue ("range");
    pFocus = apvts.getRawParameterValue ("focus");
    pListen = apvts.getRawParameterValue ("listen");
    pHeatOn = apvts.getRawParameterValue ("heatOn");
    pMode = apvts.getRawParameterValue ("mode");
    pDrive = apvts.getRawParameterValue ("drive");
    pGuard = apvts.getRawParameterValue ("guard");
    pTone = apvts.getRawParameterValue ("tone");
    pMix = apvts.getRawParameterValue ("mix");
    pLevel = apvts.getRawParameterValue ("level");
    pOutput = apvts.getRawParameterValue ("output");
    pOs = apvts.getRawParameterValue ("os");
    pLowCut = apvts.getRawParameterValue ("lowcut");
    startTimerHz (20);
}

VelvetProcessor::~VelvetProcessor() { stopTimer(); }

bool VelvetProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) && out == layouts.getMainInputChannelSet();
}

axv::Params VelvetProcessor::readParams() const
{
    axv::Params p;
    p.deessOn = pDeessOn->load() > 0.5f;
    p.amountPct = pAmount->load();
    p.rangeDb = pRange->load();
    p.focus = juce::roundToInt (pFocus->load());
    p.listen = pListen->load() > 0.5f;
    p.heatOn = pHeatOn->load() > 0.5f;
    p.mode = juce::roundToInt (pMode->load());
    p.drive = pDrive->load();
    p.guardPct = pGuard->load();
    p.tone = pTone->load();
    p.mixPct = pMix->load();
    p.levelDb = pLevel->load();
    p.outputDb = pOutput->load();
    p.osLog2 = juce::roundToInt (pOs->load()) + 1;
    p.lowCut = juce::roundToInt (pLowCut->load());
    return p;
}

void VelvetProcessor::syncEngineParams (bool bypass)
{
    auto p = readParams();
    p.bypass = bypass;
    dsp.setParams (p);
}

void VelvetProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedChannels = juce::jlimit (1, axell::kMaxCh, getMainBusNumOutputChannels());
    syncEngineParams (false);
    dsp.prepare (sampleRate, preparedChannels, samplesPerBlock);
    dsp.handshake().setNonRealtime (isNonRealtime());
    setLatencySamples (dsp.getLatencySamples());
    dsp.handshake().setHostLatency (getLatencySamples());
}

void VelvetProcessor::reset()
{
    if (preparedChannels > 0)
        dsp.reset();
}

void VelvetProcessor::setNonRealtime (bool isOffline) noexcept
{
    AudioProcessor::setNonRealtime (isOffline);
    dsp.handshake().setNonRealtime (isOffline);
}

void VelvetProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    if (preparedChannels == 0 || buffer.getNumSamples() == 0)
        return;
    syncEngineParams (false);
    dsp.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), preparedChannels), buffer.getNumSamples());
}

// host bypass: the engine keeps running (latency-aligned dry, 10 ms crossfade instead of a click)
void VelvetProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    if (preparedChannels == 0 || buffer.getNumSamples() == 0)
        return;
    syncEngineParams (true);
    dsp.process (buffer.getArrayOfWritePointers(), juce::jmin (buffer.getNumChannels(), preparedChannels), buffer.getNumSamples());
}

void VelvetProcessor::timerCallback()
{
    if (programNamePending.exchange (false))
        setPresetName (getProgramName (currentProgram.load()));
    // latency handshake: a new latency plays only after it was reported to the host (axell::LatencyHandshake)
    auto& hs = dsp.handshake();
    hs.heartbeat();
    int lat = 0;
    if (preparedChannels > 0 && hs.take (lat))
    {
        if (lat != getLatencySamples())
            setLatencySamples (lat);
        hs.ack();
    }
}

//== presets, A/B, LEARN =======================================================================================
void VelvetProcessor::applyParams (const axv::Params& p, bool includeLevel)
{
    auto set = [this] (const char* id, float v)
    {
        if (auto* prm = apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    set ("deessOn", p.deessOn ? 1.0f : 0.0f);
    set ("amount", p.amountPct);
    set ("range", p.rangeDb);
    set ("focus", (float) p.focus);
    set ("listen", 0.0f);
    set ("heatOn", p.heatOn ? 1.0f : 0.0f);
    set ("mode", (float) p.mode);
    set ("drive", p.drive);
    set ("guard", p.guardPct);
    set ("tone", p.tone);
    set ("mix", p.mixPct);
    set ("output", p.outputDb);
    set ("lowcut", (float) p.lowCut);
    if (includeLevel)
        set ("level", p.levelDb);
}

void VelvetProcessor::applyPreset (const axv::FactoryPreset& p) { applyParams (p.params, p.setsLevel); }

int VelvetProcessor::getNumPrograms() { return (int) axv::factoryPresets().size(); }

void VelvetProcessor::setCurrentProgram (int index)
{
    const auto list = axv::factoryPresets();
    if (index < 0 || index >= (int) list.size())
        return;
    currentProgram.store (index);
    applyPreset (list[(size_t) index]);
    if (juce::MessageManager::existsAndIsCurrentThread())
        setPresetName (list[(size_t) index].name);
    else
        programNamePending.store (true);
}

const juce::String VelvetProcessor::getProgramName (int index)
{
    const auto list = axv::factoryPresets();
    return index >= 0 && index < (int) list.size() ? juce::String (list[(size_t) index].name) : juce::String();
}

juce::String VelvetProcessor::getPresetName() const
{
    const juce::ScopedLock sl (nameLock);
    return presetName;
}

void VelvetProcessor::setPresetName (const juce::String& n)
{
    const juce::ScopedLock sl (nameLock);
    presetName = n;
}

juce::File VelvetProcessor::userPresetFolder()
{
    auto base = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    juce::File root = base.isNotEmpty() ? juce::File (base)
                                        : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");
    return root.getChildFile ("AxellVelvet").getChildFile ("Presets");
}

bool VelvetProcessor::saveUserPreset (const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    auto folder = userPresetFolder();
    if (clean.isEmpty() || ! folder.createDirectory())
        return false;
    auto state = apvts.copyState();
    state.setProperty (kPresetNameId, name.trim(), nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (clean + ".axvpreset")))
        return false;
    setPresetName (name.trim());
    return true;
}

bool VelvetProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;
    auto tree = juce::ValueTree::fromXml (*xml);
    restoreTree (tree);
    setPresetName (tree.getProperty (kPresetNameId, file.getFileNameWithoutExtension()).toString());
    return true;
}

void VelvetProcessor::selectSlot (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == activeSlot)
        return;
    slots[activeSlot] = apvts.copyState();
    if (slots[slot].isValid())
        restoreTree (slots[slot].createCopy());
    else
        slots[slot] = apvts.copyState();
    activeSlot = slot;
}

void VelvetProcessor::copyActiveToOther() { slots[1 - activeSlot] = apvts.copyState(); }

void VelvetProcessor::startLearn()
{
    dsp.requestMeterReset();
    learning = true;
}

float VelvetProcessor::learnSuggestionDb() const
{
    const float i = dsp.inputLoudness().integratedLufs();
    if (! learning || ! std::isfinite (i) || i < -70.0f || dsp.measuredSeconds() < 3.0)
        return std::numeric_limits<float>::quiet_NaN();
    return juce::jlimit (-24.0f, 36.0f, -18.0f - i);
}

void VelvetProcessor::applyLearn()
{
    const float s = learnSuggestionDb();
    learning = false;
    if (! std::isfinite (s))
        return;
    if (auto* prm = apvts.getParameter ("level"))
        prm->setValueNotifyingHost (prm->convertTo0to1 (s));
}

// APVTS::replaceState skips a parameter whose snapped value already matches; re-apply explicitly (pluginval scenario)
void VelvetProcessor::restoreTree (const juce::ValueTree& tree)
{
    apvts.replaceState (tree);
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto child = apvts.state.getChildWithProperty ("id", rp->paramID);
            if (child.isValid() && child.hasProperty ("value"))
            {
                const float norm = rp->convertTo0to1 ((float) child.getProperty ("value"));
                if (std::abs (rp->getValue() - norm) > 1.0e-6f)
                    rp->setValueNotifyingHost (norm);
            }
        }
}

void VelvetProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (kPresetNameId, getPresetName(), nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void VelvetProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        setPresetName (tree.getProperty (kPresetNameId, getPresetName()).toString());
        restoreTree (tree);
    }
}

juce::AudioProcessorEditor* VelvetProcessor::createEditor() { return new VelvetEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VelvetProcessor(); }
