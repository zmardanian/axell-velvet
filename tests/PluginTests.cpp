// Plugin-level tests (JUCE): parameters, state save/restore, latency reporting, presets, A/B, LEARN, bypass, editor.
// Exit code = number of failures. Registered with CTest as "plugin_suite".
#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

#include <cstdio>
#include <random>
#include <thread>

namespace
{
int failures = 0, checks = 0;
using Buf = std::vector<std::vector<float>>;

void check (bool ok, const juce::String& what)
{
    ++checks;
    if (! ok)
        ++failures;
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
}

void setParam (VelvetProcessor& p, const char* id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}
float getParam (VelvetProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

void prepare (VelvetProcessor& p, double fs = 48000.0, int block = 256, int ch = 2)
{
    p.setPlayConfigDetails (ch, ch, fs, block);
    p.prepareToPlay (fs, block);
}

// a voice-like test signal: vowels (pulse train through formants) and noise sibilants, gain in dB
Buf voice (int n, double fs, double gainDb, unsigned seed)
{
    Buf b (2, std::vector<float> ((size_t) n));
    std::mt19937 g (seed);
    std::normal_distribution<double> nd;
    const double pi = juce::MathConstants<double>::pi, gain = std::pow (10.0, gainDb / 20.0);
    double ph = 0, s1 = 0, s2 = 0;
    const double w = 2 * pi * 7000 / fs, r = std::exp (-pi * 1500 / fs);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, u = std::fmod (t, 0.42);
        double y = 0;
        if (u < 0.3)
        {
            ph += 2 * pi * 140 / fs;
            for (int h = 1; h < 30; ++h)
                y += (std::exp (-std::pow ((h * 140.0 - 650) / 250, 2)) + 0.5 * std::exp (-std::pow ((h * 140.0 - 1150) / 300, 2))) * std::sin (h * ph) / h;
            y *= 0.3 * std::min (1.0, std::min (u, 0.3 - u) / 0.02);
        }
        const double a = (1 - r) * nd (g) + 2 * r * std::cos (w) * s1 - r * r * s2;
        s2 = s1;
        s1 = a;
        if (u >= 0.3)
            y += 0.6 * a * std::sin (pi * (u - 0.3) / 0.12);
        b[0][(size_t) i] = b[1][(size_t) i] = (float) (gain * y);
    }
    return b;
}

Buf render (VelvetProcessor& p, const Buf& in, int block = 256, bool bypassed = false)
{
    const int nc = p.getTotalNumOutputChannels();
    Buf out ((size_t) nc, std::vector<float> (in[0].size()));
    juce::AudioBuffer<float> buf (nc, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos + (size_t) block <= in[0].size(); pos += (size_t) block)
    {
        for (int c = 0; c < nc; ++c)
            buf.copyFrom (c, 0, in[(size_t) c].data() + pos, block);
        if (bypassed)
            p.processBlockBypassed (buf, midi);
        else
            p.processBlock (buf, midi);
        for (int c = 0; c < nc; ++c)
            std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + block, out[(size_t) c].begin() + (long) pos);
    }
    return out;
}

void pumpMessages (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // user presets go to a temporary XDG data dir, never the real one
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("axell-velvet-plugin-tests");
    tmp.deleteRecursively();
    tmp.createDirectory();
    setenv ("XDG_DATA_HOME", tmp.getFullPathName().toRawUTF8(), 1);

    //== 1. frozen parameter IDs and order (automation uses the index), version hints, defaults
    {
        VelvetProcessor p;
        const juce::StringArray frozen { "deessOn", "amount", "range", "focus", "listen", "heatOn", "mode", "drive", "guard",
                                         "tone", "mix", "level", "output", "os", "lowcut", "deessMode" };
        auto& params = p.getParameters();
        check (params.size() == frozen.size(), "parameter count = " + juce::String (frozen.size()));
        for (int i = 0; i < params.size() && i < frozen.size(); ++i)
        {
            auto* wp = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i]);
            const int hint = i < 15 ? 1 : 3; // 1.0's fifteen, then 3.0's De-Ess Mode
            check (wp != nullptr && wp->getParameterID() == frozen[i] && wp->getVersionHint() == hint,
                   "parameter index " + juce::String (i) + " is '" + frozen[i] + "' (version hint " + juce::String (hint) + ")");
        }
        const auto d = p.readParams();
        check (d.deessOn && std::abs (d.amountPct - 50.0f) < 1e-4f && d.heatOn && d.mode == axv::kTape && std::abs (d.drive - 4.0f) < 1e-4f
                   && d.osLog2 == 4 && std::abs (d.levelDb) < 1e-4f && ! d.listen,
               "defaults: de-ess 50 %, tape at drive 4, 16x, LEVEL 0 dB, listen off");
        check (d.lowCut == 1, "default LOW CUT 30 Hz (rumble only)");
        check (! d.live, "default de-ess mode STUDIO");
    }

    //== 2. state save/restore: non-default values -> identical parameters + identical audio
    {
        VelvetProcessor a;
        const std::pair<const char*, float> values[] = { { "deessOn", 1.0f }, { "amount", 72.5f }, { "range", 21.0f }, { "focus", 1.0f },
                                                         { "listen", 0.0f }, { "heatOn", 1.0f }, { "mode", 2.0f }, { "drive", 6.3f },
                                                         { "guard", 85.0f }, { "tone", -35.0f }, { "mix", 70.0f }, { "level", 9.5f },
                                                         { "output", -1.5f }, { "os", 2.0f }, { "lowcut", 3.0f }, { "deessMode", 1.0f } };
        for (auto& v : values)
            setParam (a, v.first, v.second);
        a.setPresetName ("My Lead Vocal");
        juce::MemoryBlock state;
        a.getStateInformation (state);

        VelvetProcessor b;
        b.setStateInformation (state.getData(), (int) state.getSize());
        bool same = true;
        for (auto& v : values)
            if (std::abs (getParam (a, v.first) - getParam (b, v.first)) > 1.0e-6f)
            {
                same = false;
                std::printf ("   mismatch %s: %f vs %f\n", v.first, getParam (a, v.first), getParam (b, v.first));
            }
        check (same, "state restore: all parameter values identical");
        check (b.getPresetName() == "My Lead Vocal", "state restore: preset name restored");
        prepare (a);
        prepare (b);
        const auto sig = voice (48000 * 2, 48000.0, -16.0, 5);
        const auto oa = render (a, sig);
        const auto ob = render (b, sig);
        double diff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < oa[0].size(); ++i)
                diff = std::max (diff, (double) std::abs (oa[(size_t) c][i] - ob[(size_t) c][i]));
        check (diff <= 0.0, "state restore: restored instance renders bit-identical audio (max diff " + juce::String (diff) + ")");
        check (a.getLatencySamples() == a.engine().latencyOf ({ 3, true }) && b.getLatencySamples() == a.getLatencySamples()
                   && a.engine().latencyOf ({ 3, true }) == a.engine().latencyOf ({ 3, false }) - a.engine().latencyOf ({ 0, false }) + a.engine().latencyOf ({ 0, true }),
               "state restore: latency = LIVE de-esser (0) + 8x oversampling (" + juce::String (b.getLatencySamples()) + " samples)");
    }

    //== 2a. a 1.0 project (no De-Ess Mode in its state) opens with the STUDIO de-esser, even on an instance set to LIVE
    {
        VelvetProcessor a;
        setParam (a, "amount", 64.0f);
        juce::MemoryBlock state;
        a.getStateInformation (state);
        std::unique_ptr<juce::XmlElement> xml (juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()));
        auto tree = juce::ValueTree::fromXml (*xml);
        tree.removeChild (tree.getChildWithProperty ("id", "deessMode"), nullptr);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);
        VelvetProcessor b;
        setParam (b, "deessMode", 1.0f);
        b.setStateInformation (old.getData(), (int) old.getSize());
        check (! b.readParams().live && std::abs (getParam (b, "amount") - 64.0f) < 1e-4f,
               "1.0 state (no De-Ess Mode): restores its values and the STUDIO de-esser");
    }

    //== 2b. pluginval-style: raw (unsnapped) normalised values on every parameter, save, scramble, restore
    {
        juce::Random rng (1234);
        bool ok = true;
        for (int round = 0; round < 50 && ok; ++round)
        {
            VelvetProcessor p;
            auto& params = p.getParameters();
            for (auto* prm : params)
                prm->setValueNotifyingHost (rng.nextFloat());
            std::vector<float> saved;
            for (auto* prm : params)
                saved.push_back (prm->getValue());
            juce::MemoryBlock mb;
            p.getStateInformation (mb);
            for (auto* prm : params)
                prm->setValueNotifyingHost (rng.nextFloat());
            p.setStateInformation (mb.getData(), (int) mb.getSize());
            for (int i = 0; i < params.size(); ++i)
            {
                auto* rp = dynamic_cast<juce::RangedAudioParameter*> (params[i]);
                const float expected = rp->convertTo0to1 (rp->convertFrom0to1 (saved[(size_t) i]));
                if (std::abs (params[i]->getValue() - expected) > 0.01f)
                {
                    ok = false;
                    std::printf ("   round %d %s: %f expected %f\n", round, rp->paramID.toRawUTF8(), params[i]->getValue(), expected);
                }
            }
        }
        check (ok, "raw-value state restore (pluginval scenario), 50 random rounds");
    }

    //== 3. latency reporting follows the oversampling (handshake + message-thread timer)
    {
        VelvetProcessor p;
        prepare (p);
        check (p.getLatencySamples() == p.engine().latencyOf ({ 4 }), "prepare: latency = de-esser + 16x (" + juce::String (p.getLatencySamples()) + ")");
        const auto sig = voice (9600, 48000.0, -16.0, 1);
        bool finite = true;
        for (int os : { 1, 0, 2, 3 })
        {
            setParam (p, "os", (float) os);
            for (int k = 0; k < 4; ++k)
            {
                const auto out = render (p, sig);
                for (auto& ch : out)
                    for (float v : ch)
                        finite = finite && std::isfinite (v) && std::abs (v) < 2.0f;
                pumpMessages (60);
            }
            const int expect = p.engine().latencyOf ({ os + 1 });
            check (p.getLatencySamples() == expect && p.engine().getLatencySamples() == expect,
                   "after switching to " + juce::String (2 << os) + "x: reported latency " + juce::String (p.getLatencySamples()) + " = " + juce::String (expect));
        }
        check (finite, "every sample during the switches finite and in range");
        // 3.0: LIVE (the de-esser's latency goes) and LIVE with HEAT off (no latency at all), and back to STUDIO
        struct Step { float mode, heat; axv::Engine::Config cfg; const char* what; };
        const Step steps[] = { { 1.0f, 1.0f, { 4, true, true }, "LIVE, heat on: latency = 16x oversampler only" },
                               { 1.0f, 0.0f, { 4, true, false }, "LIVE, heat off: latency 0" },
                               { 0.0f, 0.0f, { 4, false, true }, "back to STUDIO, heat off: de-esser + 16x" } };
        setParam (p, "os", 3.0f);
        for (const auto& st : steps)
        {
            setParam (p, "deessMode", st.mode);
            setParam (p, "heatOn", st.heat);
            for (int k = 0; k < 4; ++k)
            {
                const auto out = render (p, sig);
                for (auto& ch : out)
                    for (float v : ch)
                        finite = finite && std::isfinite (v) && std::abs (v) < 2.0f;
                pumpMessages (60);
            }
            const int expect = p.engine().latencyOf (st.cfg);
            check (p.getLatencySamples() == expect && p.engine().getLatencySamples() == expect,
                   juce::String (st.what) + " (reported " + juce::String (p.getLatencySamples()) + ", expected " + juce::String (expect) + ")");
        }
        check (finite && p.engine().latencyOf ({ 4, true, false }) == 0, "LIVE switches: every sample finite; LIVE + heat off is 0 samples");
    }

    //== 4. factory programs
    {
        VelvetProcessor p;
        const auto list = axv::factoryPresets();
        check (p.getNumPrograms() == 28, "programs: " + juce::String (p.getNumPrograms()) + " factory presets (20 + 2 TRACKING + 6 MY VOICE)");
        bool allOk = true;
        int mine = 0;
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            setParam (p, "level", 3.0f);
            p.setCurrentProgram (i);
            const auto r = p.readParams();
            const auto& pr = list[(size_t) i];
            const auto& e = pr.params;
            const float expectLevel = pr.setsLevel ? e.levelDb : 3.0f;
            mine += pr.setsLevel ? 1 : 0;
            if (std::abs (r.amountPct - e.amountPct) > 0.051f || std::abs (r.rangeDb - e.rangeDb) > 0.051f || r.focus != e.focus
                || r.heatOn != e.heatOn || r.deessOn != e.deessOn || r.mode != e.mode || std::abs (r.drive - e.drive) > 0.0051f
                || std::abs (r.guardPct - e.guardPct) > 0.51f || std::abs (r.tone - e.tone) > 0.51f || std::abs (r.mixPct - e.mixPct) > 0.51f
                || r.osLog2 != e.osLog2 || std::abs (r.levelDb - expectLevel) > 0.051f || r.listen || r.lowCut != e.lowCut || r.live != e.live)
            {
                allOk = false;
                std::printf ("   program %d (%s) did not apply\n", i, pr.name);
            }
        }
        check (allOk, "every program applies its values; factory presets keep LEVEL, MY VOICE presets set it");
        check (mine == 6, "6 MY VOICE presets set LEVEL");
        setParam (p, "level", 0.0f);
        p.setCurrentProgram (0);
        bool defaults = true;
        for (auto* prm : p.getParameters())
            if (std::abs (prm->getValue() - prm->getDefaultValue()) > 1.0e-4f)
            {
                defaults = false;
                std::printf ("   Init: %s not default\n", prm->getName (40).toRawUTF8());
            }
        check (defaults, "'Init' program = all parameter defaults");
        std::thread t ([&p] { p.setCurrentProgram (4); });
        t.join();
        check (p.getCurrentProgram() == 4 && p.readParams().mode == list[4].params.mode, "program change from another thread applies the values");
        pumpMessages (150);
        check (p.getPresetName() == list[4].name, "program change from another thread: name updated by the timer");
        // whole words only ("never" is not a brand)
        const juce::StringArray brands { "lewitt", "lct", "volt", "universal", "ua", "uad", "1176", "neve", "ssl", "api", "studer", "ampex",
                                         "pultec", "fabfilter", "waves", "soothe", "oeksound", "teletronix", "fairchild", "telefunken" };
        bool brand = false;
        for (const auto& pr : list)
        {
            juce::StringArray words;
            words.addTokens (juce::String (pr.name) + " " + pr.description, " -,.:;()/'", "");
            for (const auto& w : words)
                if (brands.contains (w.toLowerCase()))
                {
                    brand = true;
                    std::printf ("   brand word '%s' in '%s'\n", w.toRawUTF8(), pr.name);
                }
        }
        check (! brand, "no third-party brand or product names in preset names or descriptions");
    }

    //== 5. user presets save/load
    {
        VelvetProcessor p;
        setParam (p, "amount", 33.0f);
        setParam (p, "mode", 1.0f);
        check (p.saveUserPreset ("Test / Vocal: 1"), "user preset saved (name with illegal file characters)");
        auto files = VelvetProcessor::userPresetFolder().findChildFiles (juce::File::findFiles, false, "*.axvpreset");
        check (files.size() == 1 && VelvetProcessor::userPresetFolder().isAChildOf (tmp), "user preset written to $XDG_DATA_HOME/AxellVelvet/Presets");
        VelvetProcessor q;
        check (files.size() == 1 && q.loadUserPreset (files[0]), "user preset loads");
        check (std::abs (getParam (q, "amount") - 33.0f) < 1e-4f && std::abs (getParam (q, "mode") - 1.0f) < 1e-4f
                   && q.getPresetName() == "Test / Vocal: 1",
               "user preset restores values and name");
    }

    //== 6. A/B compare
    {
        VelvetProcessor p;
        setParam (p, "drive", 2.0f);
        p.selectSlot (1);
        setParam (p, "drive", 8.0f);
        p.selectSlot (0);
        const float a = getParam (p, "drive");
        p.selectSlot (1);
        const float b = getParam (p, "drive");
        check (std::abs (a - 2.0f) < 1e-4f && std::abs (b - 8.0f) < 1e-4f, "A/B: each slot keeps its own settings");
        p.copyActiveToOther();
        p.selectSlot (0);
        check (std::abs (getParam (p, "drive") - 8.0f) < 1e-4f, "A/B: copy B > A");
    }

    //== 7. LEARN: a quiet vocal gets LEVEL = -18 LUFS minus its integrated loudness
    {
        VelvetProcessor p;
        prepare (p);
        const auto sig = voice (48000 * 6, 48000.0, -32.0, 9);
        p.startLearn();
        render (p, Buf (2, std::vector<float> (2048, 0.0f)));
        check (! std::isfinite (p.learnSuggestionDb()), "LEARN: no suggestion before 3 s of audio");
        render (p, sig);
        const float s = p.learnSuggestionDb();
        const float i = p.engine().inputLoudness().integratedLufs();
        check (std::isfinite (s) && std::abs (s - (-18.0f - i)) < 0.01f,
               "LEARN: suggestion = -18 - integrated (" + juce::String (i, 2) + " LUFS -> LEVEL " + juce::String (s, 2) + " dB)");
        p.applyLearn();
        check (std::abs (getParam (p, "level") - s) < 0.051f && ! p.isLearning(), "LEARN: applied to LEVEL");
    }

    //== 8. host bypass keeps the latency alignment (dry, delayed by the reported latency)
    {
        VelvetProcessor p;
        prepare (p);
        setParam (p, "drive", 9.0f);
        const auto sig = voice (48000, 48000.0, -14.0, 4);
        const auto out = render (p, sig, 256, true);
        const int L = p.getLatencySamples();
        double err = 0;
        for (size_t i = (size_t) L + 24000; i < sig[0].size() - 512; ++i)
            err = std::max (err, (double) std::abs (out[0][i] - sig[0][i - (size_t) L]));
        check (err < 1.0e-6, "host bypass: dry output delayed by the reported latency (error " + juce::String (err) + ")");
    }

    //== 9. mono track (1 in / 1 out)
    {
        VelvetProcessor p;
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add (juce::AudioChannelSet::mono());
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        check (p.setBusesLayout (mono), "mono layout accepted");
        prepare (p, 44100.0, 300, 1);
        const auto sig = voice (44100 * 2, 44100.0, -14.0, 7);
        const auto out = render (p, sig, 300);
        double pk = 0;
        bool fin = true;
        for (float v : out[0])
        {
            fin = fin && std::isfinite (v);
            pk = std::max (pk, (double) std::abs (v));
        }
        check (fin && pk > 0.01 && pk < 1.5, "mono: processes (peak " + juce::String (pk, 3) + ")");
    }

    //== 10. editor: open/close repeatedly while processing, size limits and aspect ratio
    {
        VelvetProcessor p;
        prepare (p);
        const auto sig = voice (2048, 48000.0, -14.0, 2);
        bool ok = true;
        for (int i = 0; i < 5; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            render (p, sig);
            pumpMessages (40);
            ok = ok && ed->getWidth() == vui::kWidth;
        }
        check (ok, "editor: opens/closes 5x while processing (100 % size)");
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto* c = ed->getConstrainer();
        check (c != nullptr && c->getMinimumWidth() == vui::kWidth * 3 / 4 && c->getMaximumWidth() == 2 * vui::kWidth
                   && std::abs (c->getFixedAspectRatio() - (double) vui::kWidth / vui::kHeight) < 1e-9,
               "editor: resizable 75-200 %, aspect locked");
        ed->setSize (vui::kWidth * 3 / 2, vui::kHeight * 3 / 2);
        ed.reset();
        std::unique_ptr<juce::AudioProcessorEditor> ed2 (p.createEditor());
        check (ed2->getWidth() == vui::kWidth * 3 / 2, "editor: re-opens at the last used size (150 %)");
    }

    tmp.deleteRecursively();
    std::printf ("%s: %d of %d plugin checks passed\n", failures == 0 ? "ALL PASS" : "FAILURES", checks - failures, checks);
    return failures;
}
