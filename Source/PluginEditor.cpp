#include "PluginEditor.h"

#include <axell/gui/BrandTitle.h>

#include <cmath>

using namespace ui;

namespace
{
const juce::Identifier kUiWidth ("uiWidth");
namespace tip
{
const char* deessOn = "De-esser on / off. Off keeps the timing (latency) the same, so switching never moves the vocal.";
const char* amount = "How far the esses come down. The de-esser recognises sibilants (s, z, sh, ch, j) by their sound, not "
                     "their level, and lets their treble reach the level of your voice's body plus a margin: 50 % = 6 dB "
                     "below it. Only the frequencies where each ess is harsh are lowered; everything else is untouched. "
                     "It works the same on a quiet verse and a loud chorus, so there is no threshold to set.";
const char* range = "The most any frequency band may be cut. 12-18 dB for natural results; higher only for very harsh "
                    "recordings.";
const char* focus = "AUTO: every ess where it is harsh (default). S: only the sharp hiss above 5.6 kHz. SH: only the "
                    "broad 'sh / ch' region below 5.6 kHz.";
const char* listen = "Hear only what the de-esser removes. You should hear esses and nothing else.";
const char* deessMode = "STUDIO (default): looks 4 ms ahead and cuts with linear-phase bands - the most precise, for mixing "
                        "(the host compensates its 14 ms). LIVE: zero latency, for recording while you hear yourself "
                        "through the plugin; it reacts a few milliseconds later to each ess.";
const char* sib = "The last seconds of your vocal's treble (2-17 kHz) against its body: bright where the de-esser is "
                  "sure it is an ess, grey otherwise. The dashed line is the AMOUNT target; the teal area from the top "
                  "is the reduction.";
const char* heatOn = "Heat on / off (the timing stays the same).";
const char* mode = "TAPE: magnetic tape - smooth, glued; treble and esses saturate first, the low end thickens. "
                   "CONSOLE: transformer and class-A transistor stage - punch, weight, presence. "
                   "TUBE: a triode stage - rich, sweet even harmonics that bloom on loud notes.";
const char* drive = "How hard the vocal hits the model. The level is matched automatically, so turn it up for character, "
                    "not volume: 0-3 subtle, 4-6 warm, 7-10 driven.";
const char* guard = "S-GUARD: the esses get less drive (100 % = none), so the warmth never makes them harsher. Uses the "
                    "de-esser's detector, so it works even when the de-esser itself is off.";
const char* tone = "Final colour of the heated sound: darker (smoother top) or brighter (more air). A gentle shelf at "
                   "4.5 kHz, up to 4 dB.";
const char* mix = "Blend of the heated and the clean (de-essed) vocal: parallel saturation.";
const char* level = "Where your vocal sits against the models' nominal level (-18 LUFS). Recorded quietly? Press LEARN "
                    "while the vocal plays: LEVEL is set for you, so DRIVE means the same on every recording. LEVEL "
                    "does not change the output volume.";
const char* learn = "LEARN: play the vocal (at least 3 s), then press APPLY.";
const char* output = "Output volume.";
const char* lowCut = "LOW CUT before everything (24 dB/oct, Hz). 30 removes only inaudible rumble (stand, floor, air), which would "
                     "otherwise make the HEAT models distort your voice with it. 60-90 for vocals in a mix.";
const char* os = "Oversampling of the HEAT models. 16x (default): even full drive on bright vocals stays clean. Lower "
                 "settings save CPU.";
const char* heatView = "The model's curve at your vocal's current level: input left to right, output bottom to top. "
                       "Straight = clean; the bend is the saturation (tape draws its magnetic loop).";
} // namespace
} // namespace

//==============================================================================
SibilanceView::SibilanceView() : ring (2000) {}

void SibilanceView::push (const axv::DisplayFrame& f)
{
    ring[(size_t) pos] = f;
    pos = (pos + 1) % (int) ring.size();
}

void SibilanceView::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto r = getLocalBounds().toFloat();
    g.setColour (col::well);
    g.fillRoundedRectangle (r, 10.0f);
    const auto plot = r.reduced (12.0f, 10.0f).withTrimmedLeft (24.0f).withTrimmedBottom (14.0f);
    const float lo = -36.0f, hi = 12.0f;
    auto yL = [&] (float d) { return plot.getBottom() - (jlimit (lo, hi, d) - lo) / (hi - lo) * plot.getHeight(); };
    auto yG = [&] (float d) { return plot.getY() + jlimit (0.0f, 24.0f, d) / 24.0f * plot.getHeight() * 0.45f; };
    g.setFont (ui::font (10.0f));
    for (int k = 1; 6.0f * (float) k > lo; --k)
    {
        const float d = 6.0f * (float) k;
        g.setColour (k == 0 ? col::faint : col::grid);
        g.drawHorizontalLine ((int) yL (d), plot.getX(), plot.getRight());
        g.setColour (col::faint);
        g.drawText ((d > 0 ? "+" : "") + String ((int) d), Rectangle<float> (r.getX() + 4.0f, yL (d) - 6.0f, 28.0f, 12.0f), Justification::right);
    }
    const int n = (int) plot.getWidth(), N = (int) ring.size();
    // one column per pixel: the frames it covers (N / n), max level, max reduction, max confidence
    const float per = (float) N / (float) n;
    float target = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const int a = (int) (i * per), b = std::max (a + 1, (int) ((i + 1) * per));
        float lvl = -120.0f, gr = 0.0f, p = 0.0f;
        for (int k = a; k < b; ++k)
        {
            const auto& f = ring[(size_t) ((pos + k) % N)];
            lvl = std::max (lvl, f.hfReBodyDb);
            gr = std::max (gr, f.grDb);
            p = std::max (p, f.p);
            target = f.targetDb;
        }
        const float x = plot.getX() + (float) i;
        if (lvl > lo)
        {
            g.setColour (col::dim.withAlpha (0.35f).interpolatedWith (col::teal, active ? p : 0.0f));
            g.drawVerticalLine ((int) x, yL (lvl), plot.getBottom());
            if (gr > 0.05f && lvl - gr > lo)
            {
                g.setColour (col::text.withAlpha (0.7f));
                g.fillRect (x, yL (lvl - gr) - 1.0f, 1.0f, 2.0f);
            }
        }
        if (gr > 0.05f)
        {
            g.setColour (col::teal.withAlpha (0.65f));
            g.drawVerticalLine ((int) x, plot.getY(), yG (gr));
        }
    }
    if (active)
    {
        Path d, dashed;
        const float y = yL (target);
        d.startNewSubPath (plot.getX(), y);
        d.lineTo (plot.getRight(), y);
        float dash[] = { 5.0f, 4.0f };
        PathStrokeType (1.2f).createDashedStroke (dashed, d, dash, 2);
        g.setColour (col::warn.withAlpha (0.8f));
        g.fillPath (dashed);
        g.setFont (ui::font (10.0f, 1));
        g.drawText ("TARGET", Rectangle<float> (plot.getRight() - 70.0f, y - 14.0f, 66.0f, 12.0f), Justification::right);
    }
    g.setFont (ui::caps (9.5f, 1));
    auto lg = Rectangle<float> (plot.getX(), plot.getBottom() + 3.0f, 560.0f, 12.0f);
    auto item = [&] (Colour c, const String& t)
    {
        g.setColour (c);
        g.fillRoundedRectangle (lg.removeFromLeft (10.0f).reduced (0.0f, 2.0f), 2.0f);
        lg.removeFromLeft (5.0f);
        g.setColour (col::dim);
        const float w = (float) GlyphArrangement::getStringWidthInt (g.getCurrentFont(), t) + 14.0f;
        g.drawText (t, lg.removeFromLeft (w), Justification::centredLeft);
    };
    item (col::teal, "ESS (DETECTED)");
    item (col::dim.withAlpha (0.6f), "OTHER TREBLE");
    item (col::text.withAlpha (0.7f), "AFTER");
    item (col::teal.withAlpha (0.65f), "REDUCTION");
    g.setColour (col::faint);
    g.drawText ("dB RE VOICE BODY", Rectangle<float> (plot.getRight() - 160.0f, plot.getBottom() + 3.0f, 160.0f, 12.0f), Justification::right);
}

//==============================================================================
void HeatView::update (const axv::Params& p, float inPeakDb, float guardWeight)
{
    if (inPeakDb >= peakHold) { peakHold = inPeakDb; hold = 20; }
    else if (--hold <= 0) peakHold = std::max (-120.0f, peakHold - 0.6f);
    guardShown = guardWeight < guardShown ? guardWeight : guardShown + 0.08f * (guardWeight - guardShown);
    // drive gain into the model at the held peak (no S-GUARD: the curve shows the vowels)
    const float eff = p.levelDb + axv::Engine::kNominalOffsetDb + axv::Engine::driveToDb (p.mode, p.drive);
    const float amp = std::pow (10.0f, (std::max (peakHold, -60.0f) + eff) / 20.0f);
    if (p.mode != lastMode || std::abs (amp - lastAmp) > 0.02f * std::max (lastAmp, 1.0e-3f))
    {
        params = p;
        lastMode = p.mode;
        lastAmp = amp;
        compute();
    }
    repaint();
}

void HeatView::compute()
{
    const double fo = 48000.0 * 8.0, f = 100.0;
    const int per = (int) (fo / f);
    std::vector<float> x ((size_t) per * 3), in ((size_t) per * 3);
    for (int i = 0; i < per * 3; ++i)
        in[(size_t) i] = x[(size_t) i] = (float) (lastAmp * std::sin (2.0 * axell::kPi * f * i / fo));
    switch (params.mode)
    {
        case axv::kConsole: { axv::ConsoleModel m; m.prepare (fo); m.process (x.data(), per * 3, 0); break; }
        case axv::kTube: { axv::TubeModel m; m.prepare (fo); m.process (x.data(), per * 3, 0); break; }
        default: { axv::TapeModel m; m.prepare (fo); m.process (x.data(), per * 3, 0); break; }
    }
    loop.clear();
    const float a = std::max (lastAmp, 1.0e-6f);
    for (int i = per * 2; i < per * 3; i += 4)
        loop.push_back ({ in[(size_t) i] / a, x[(size_t) i] / a });
    // harmonic levels of the last period (exact bins: the period is an integer number of samples)
    double h1 = 0.0;
    for (int k = 1; k <= 10; ++k)
    {
        double re = 0.0, im = 0.0;
        for (int i = per * 2; i < per * 3; ++i)
        {
            const double ph = 2.0 * axell::kPi * k * (i - per * 2) / per;
            re += x[(size_t) i] * std::cos (ph);
            im -= x[(size_t) i] * std::sin (ph);
        }
        const double mag = std::sqrt (re * re + im * im);
        if (k == 1)
            h1 = std::max (mag, 1.0e-30);
        else
            harm[(size_t) k - 2] = (float) (20.0 * std::log10 (std::max (mag / h1, 1.0e-9)));
    }
}

void HeatView::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto r = getLocalBounds().toFloat();
    g.setColour (col::well);
    g.fillRoundedRectangle (r, 10.0f);
    auto area = r.reduced (14.0f, 12.0f).withTrimmedBottom (18.0f);
    // left: the transfer (input -> output) at the live level
    auto plot = area.removeFromLeft (area.getHeight()).reduced (4.0f);
    auto px = [&] (float v) { return plot.getCentreX() + v * plot.getWidth() * 0.47f; };
    auto py = [&] (float v) { return plot.getCentreY() - v * plot.getHeight() * 0.47f; };
    g.setColour (col::grid);
    g.drawRect (plot, 1.0f);
    g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
    g.drawVerticalLine ((int) plot.getCentreX(), plot.getY(), plot.getBottom());
    {
        Path d, dashed;
        d.startNewSubPath (px (-1.0f), py (-1.0f));
        d.lineTo (px (1.0f), py (1.0f));
        float dash[] = { 3.0f, 3.0f };
        PathStrokeType (1.0f).createDashedStroke (dashed, d, dash, 2);
        g.setColour (col::faint);
        g.fillPath (dashed);
    }
    const Colour c = params.heatOn ? col::accent : col::dim;
    if (! loop.empty())
    {
        Path p;
        p.startNewSubPath (px (loop[0].x), py (loop[0].y));
        for (const auto& q : loop)
            p.lineTo (px (q.x), py (jlimit (-1.06f, 1.06f, q.y)));
        p.closeSubPath();
        g.setColour (c);
        g.strokePath (p, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
    }
    // right: harmonics H2..H10 (dBc), -100 .. 0
    area.removeFromLeft (18.0f);
    auto bars = area.withTrimmedTop (4.0f).withTrimmedBottom (14.0f);
    const float lo = -100.0f;
    auto yb = [&] (float d) { return bars.getBottom() - (jlimit (lo, 0.0f, d) - lo) / -lo * bars.getHeight(); };
    g.setFont (ui::font (9.5f));
    for (float d = -20.0f; d >= lo; d -= 20.0f)
    {
        g.setColour (col::grid);
        g.drawHorizontalLine ((int) yb (d), bars.getX() + 26.0f, bars.getRight());
        g.setColour (col::faint);
        g.drawText (String ((int) d), Rectangle<float> (bars.getX(), yb (d) - 6.0f, 24.0f, 12.0f), Justification::right);
    }
    const float bx = bars.getX() + 30.0f, bw = (bars.getRight() - bx) / 9.0f;
    for (int k = 0; k < 9; ++k)
    {
        const float x0 = bx + bw * (float) k + 3.0f, top = yb (harm[(size_t) k]);
        g.setColour (k % 2 == 0 ? c : c.withAlpha (0.55f)); // even harmonics (H2, H4..) solid, odd lighter
        if (top < bars.getBottom() - 1.0f)
            g.fillRoundedRectangle (x0, top, bw - 6.0f, bars.getBottom() - top, 2.0f);
        g.setColour (col::dim);
        g.drawText (String (k + 2), Rectangle<float> (x0, bars.getBottom() + 1.0f, bw - 6.0f, 12.0f), Justification::centred);
    }
    g.setFont (ui::caps (9.5f, 1));
    g.setColour (col::dim);
    const auto foot = r.reduced (14.0f, 6.0f).removeFromBottom (14.0f);
    g.drawText ("CURVE AT YOUR LEVEL", foot, Justification::centredLeft);
    g.setColour (guardShown < 0.95f ? col::teal : col::dim);
    g.drawText (guardShown < 0.95f ? "S-GUARD  " + String (roundToInt (100.0f * (1.0f - guardShown))) + " % ON THIS ESS" : String ("HARMONICS (dBc)"),
                foot, Justification::centredRight);
}

//==============================================================================
VelvetEditor::VelvetEditor (VelvetProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    const int savedWidth = (int) proc.apvts.state.getProperty (kUiWidth, vui::kWidth);
    setLookAndFeel (&lnf);
    addAndMakeVisible (content);
    deessOn.bind (proc.apvts, "deessOn", tip::deessOn);
    amount.bind (proc.apvts, "amount", tip::amount);
    range.bind (proc.apvts, "range", tip::range);
    focus.bind (*proc.apvts.getParameter ("focus"), tip::focus);
    deessMode.bind (*proc.apvts.getParameter ("deessMode"), tip::deessMode);
    listen.bind (proc.apvts, "listen", tip::listen);
    heatOn.bind (proc.apvts, "heatOn", tip::heatOn);
    mode.bind (*proc.apvts.getParameter ("mode"), tip::mode);
    drive.bind (proc.apvts, "drive", tip::drive);
    guard.bind (proc.apvts, "guard", tip::guard);
    tone.bind (proc.apvts, "tone", tip::tone);
    mix.bind (proc.apvts, "mix", tip::mix);
    level.bind (proc.apvts, "level", tip::level);
    output.bind (proc.apvts, "output", tip::output);
    os.bind (*proc.apvts.getParameter ("os"), tip::os);
    lowCut.bind (*proc.apvts.getParameter ("lowcut"), tip::lowCut);
    sibView.setTooltip (tip::sib);
    heatView.setTooltip (tip::heatView);
    learn.setTooltip (tip::learn);
    inMeter.setTooltip ("Input peak level.");
    outMeter.setTooltip ("Output peak level.");
    learn.onClick = [this]
    {
        if (! proc.isLearning())
            proc.startLearn();
        else
            proc.applyLearn();
        timerCallback();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &presetPrev, &presetNext, &presetName, &presetSave, &slotA, &slotB, &slotCopy,
                                                            &deessOn, &deessMode, &sibView, &amount, &range, &focus, &listen, &heatOn, &mode,
                                                            &heatView, &drive, &guard, &tone, &mix, &level, &learn, &output, &os,
                                                            &inMeter, &outMeter, &lowCut })
        content.addAndMakeVisible (c);
    for (auto* b : { &slotA, &slotB })
        b->setColour (juce::TextButton::buttonOnColourId, col::global);
    presetPrev.onClick = [this] { proc.setCurrentProgram ((proc.getCurrentProgram() + proc.getNumPrograms() - 1) % proc.getNumPrograms()); };
    presetNext.onClick = [this] { proc.setCurrentProgram ((proc.getCurrentProgram() + 1) % proc.getNumPrograms()); };
    presetName.onClick = [this] { showPresetMenu(); };
    presetSave.onClick = [this] { promptSavePreset(); };
    slotA.onClick = [this] { proc.selectSlot (0); };
    slotB.onClick = [this] { proc.selectSlot (1); };
    slotCopy.onClick = [this] { proc.copyActiveToOther(); };
    presetName.setTooltip ("Presets by vocal style, plus the MY VOICE set made for Axell's voice and recording chain. Factory "
                           "presets keep your LEVEL. Your own presets live in ~/.local/share/AxellVelvet/Presets.");
    slotA.setTooltip ("A/B: two complete settings to compare.");
    slotB.setTooltip ("A/B: two complete settings to compare.");
    slotCopy.setTooltip ("Copy the current slot to the other one.");

    setResizable (true, true);
    setResizeLimits (vui::kWidth * 3 / 4, vui::kHeight * 3 / 4, vui::kWidth * 2, vui::kHeight * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) vui::kWidth / vui::kHeight);
    const int w = juce::jlimit (vui::kWidth * 3 / 4, vui::kWidth * 2, savedWidth);
    setSize (w, juce::roundToInt (w * (double) vui::kHeight / vui::kWidth));
    constructed = true;
    timerCallback();
    startTimerHz (30);
}

VelvetEditor::~VelvetEditor()
{
    stopTimer();
    saveDialog.reset();
    setLookAndFeel (nullptr);
}

void VelvetEditor::paint (juce::Graphics& g) { g.fillAll (col::bg); }

void VelvetEditor::resized()
{
    content.setBounds (0, 0, vui::kWidth, vui::kHeight);
    content.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) vui::kWidth));
    if (constructed)
        proc.apvts.state.setProperty (kUiWidth, getWidth(), nullptr);
}

void VelvetEditor::Content::resized() { owner.layout(); }

void VelvetEditor::layout()
{
    using juce::Rectangle;
    const int m = 16;
    headerArea = { 0, 0, vui::kWidth, 58 };
    {
        auto pb = Rectangle<int> (360, 14, 400, 30);
        presetPrev.setBounds (pb.removeFromLeft (30));
        pb.removeFromLeft (4);
        presetSave.setBounds (pb.removeFromRight (62));
        pb.removeFromRight (4);
        presetNext.setBounds (pb.removeFromRight (30));
        pb.removeFromRight (4);
        presetName.setBounds (pb);
        auto ab = Rectangle<int> (vui::kWidth - m - 160, 14, 160, 30);
        slotA.setBounds (ab.removeFromLeft (34));
        ab.removeFromLeft (4);
        slotB.setBounds (ab.removeFromLeft (34));
        ab.removeFromLeft (8);
        slotCopy.setBounds (ab);
    }
    const int top = 70, bottomH = 128, midH = vui::kHeight - top - bottomH - m - 10;
    const int colW = (vui::kWidth - 2 * m - 10) / 2;
    deessArea = { m, top, colW, midH };
    heatArea = { m + colW + 10, top, colW, midH };
    bottomArea = { m, top + midH + 10, vui::kWidth - 2 * m, bottomH };
    {
        auto a = deessArea.reduced (14, 0).withTrimmedTop (44).withTrimmedBottom (12);
        deessOn.setBounds (deessArea.getRight() - 46, deessArea.getY() + 10, 30, 24);
        deessMode.setBounds (deessArea.getRight() - 46 - 10 - 132, deessArea.getY() + 10, 132, 24);
        sibView.setBounds (a.removeFromTop (220));
        a.removeFromTop (12);
        amount.setBounds (a.removeFromLeft (180));
        a.removeFromLeft (10);
        auto right = a;
        focusLabel = right.removeFromTop (16);
        focus.setBounds (right.removeFromTop (28));
        right.removeFromTop (12);
        auto row = right.removeFromTop (110);
        range.setBounds (row.removeFromLeft (row.getWidth() / 2));
        row.removeFromLeft (6);
        listen.setBounds (row.withSizeKeepingCentre (row.getWidth(), 34));
    }
    {
        auto a = heatArea.reduced (14, 0).withTrimmedTop (44).withTrimmedBottom (12);
        heatOn.setBounds (heatArea.getRight() - 46, heatArea.getY() + 10, 30, 24);
        modeLabel = a.removeFromTop (16);
        mode.setBounds (a.removeFromTop (28));
        a.removeFromTop (10);
        heatView.setBounds (a.removeFromTop (182));
        a.removeFromTop (12);
        drive.setBounds (a.removeFromLeft (180));
        a.removeFromLeft (6);
        const int w = a.getWidth() / 3;
        for (auto* k : { &guard, &tone, &mix })
            k->setBounds (a.removeFromLeft (w));
    }
    {
        auto a = bottomArea.reduced (14, 10);
        outMeter.setBounds (a.removeFromRight (44));
        a.removeFromRight (6);
        inMeter.setBounds (a.removeFromRight (44));
        a.removeFromRight (16);
        output.setBounds (a.removeFromRight (96));
        a.removeFromRight (16);
        auto osA = a.removeFromRight (200);
        osLabel = osA.removeFromTop (34).withTrimmedTop (18);
        os.setBounds (osA.removeFromTop (28));
        a.removeFromRight (14);
        auto lcA = a.removeFromRight (180);
        lowCutLabel = lcA.removeFromTop (34).withTrimmedTop (18);
        lowCut.setBounds (lcA.removeFromTop (28));
        level.setBounds (a.removeFromLeft (96));
        a.removeFromLeft (10);
        auto l = a.removeFromLeft (110);
        learn.setBounds (l.withSizeKeepingCentre (110, 30).translated (0, -12));
        learnText = a.withTrimmedTop (20);
    }
}

void VelvetEditor::Content::paint (juce::Graphics& g)
{
    using namespace juce;
    auto& e = owner;
    g.fillAll (col::bg);
    g.setColour (col::header);
    g.fillRect (e.headerArea);
    axell::brand::drawHeaderRule (g, (float) e.headerArea.getBottom() - 1.0f, 0.0f, (float) vui::kWidth, col::grid);
    // Axell's Dojo: the family emblem, its wave and dot in Velvet's teal
    axell::brand::drawEmblem (g, { 12.0f, 6.0f, 46.0f, 46.0f }, col::teal);
    axell::brand::drawTitle (g, 68.0f, 28.0f, "Velvet", juce::String (juce::CharPointer_UTF8 ("\xe7\xb5\xb9"))); // 絹
    g.setFont (ui::caps (10.0f, 1));
    g.setColour (col::dim);
    g.drawText ("SMART DE-ESSER  +  ANALOG HEAT", 67, 37, 320, 14, Justification::centredLeft);
    auto panel = [&g] (Rectangle<int> a, Colour accent, const String& title, const String& sub, bool on)
    {
        const auto r = a.toFloat();
        g.setGradientFill (ColourGradient (col::panelTop, 0, r.getY(), col::panel, 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (accent.withAlpha (on ? 0.5f : 0.15f));
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.2f);
        g.setColour (on ? col::text : col::dim);
        g.setFont (ui::caps (13.0f, 2));
        g.drawText (title, a.getX() + 16, a.getY() + 10, 300, 22, Justification::centredLeft);
        g.setColour (accent.withAlpha (on ? 0.9f : 0.3f));
        g.fillRoundedRectangle (r.getX() + 14.0f, r.getY() + 35.0f, 26.0f, 2.0f, 1.0f);
        if (sub.isNotEmpty())
        {
            g.setColour (col::faint);
            g.setFont (ui::caps (9.5f, 1));
            g.drawText (sub, a.getX() + 120, a.getY() + 10, a.getWidth() - 180, 22, Justification::centredRight);
        }
    };
    const auto p = e.proc.readParams();
    panel (e.deessArea, col::teal, "DE-ESS", p.listen ? "LISTENING TO WHAT IS REMOVED" : "", p.deessOn);
    panel (e.heatArea, col::accent, "HEAT", "", p.heatOn);
    {
        const auto r = e.bottomArea.toFloat();
        g.setGradientFill (ColourGradient (col::panelTop, 0, r.getY(), col::panel, 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (col::global.withAlpha (0.35f));
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.2f);
    }
    g.setColour (col::dim);
    g.setFont (ui::caps (10.5f, 1));
    g.drawText ("FOCUS", e.focusLabel, Justification::centredLeft);
    g.drawText ("MODEL", e.modeLabel, Justification::centredLeft);
    g.drawText ("OVERSAMPLING (HEAT)", e.osLabel, Justification::centredLeft);
    g.drawText ("LOW CUT (HZ)", e.lowCutLabel, Justification::centredLeft);
    g.setFont (ui::font (12.0f));
    g.setColour (e.proc.isLearning() ? col::warn : col::dim);
    g.drawFittedText (e.learnStatus, e.learnText, Justification::topLeft, 3);
    g.setFont (ui::font (11.0f));
    g.setColour (col::faint);
    g.drawText ("latency " + String (e.proc.getLatencySamples()) + " samples ("
                    + String (1000.0 * e.proc.getLatencySamples() / std::max (1.0, e.proc.engine().sampleRate()), 1) + " ms, compensated)",
                e.lowCutLabel.withY (e.lowCutLabel.getY() + 64).withHeight (16).withRight (e.osLabel.getRight()), Justification::centredLeft);
}

void VelvetEditor::timerCallback()
{
    axv::DisplayFrame f;
    float in = -120.0f, out = -120.0f, guardMin = 1.0f, gr = 0.0f;
    bool any = false;
    while (proc.engine().popDisplay (f))
    {
        sibView.push (f);
        in = std::max (in, f.inPeakDb);
        out = std::max (out, f.outPeakDb);
        guardMin = std::min (guardMin, f.guard);
        gr = std::max (gr, f.grDb);
        any = true;
    }
    const auto params = proc.readParams();
    sibView.setActive (params.deessOn);
    if (any)
        sibView.repaint();
    auto lin = [] (float db) { return db > -119.0f ? std::pow (10.0f, db / 20.0f) : 0.0f; };
    inMeter.push (lin (in), lin (in), 2);
    outMeter.push (lin (out), lin (out), 2);
    heatView.update (params, in, guardMin);
    presetName.setButtonText (proc.getPresetName());
    slotA.setToggleState (proc.getActiveSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState (proc.getActiveSlot() == 1, juce::dontSendNotification);
    slotCopy.setButtonText (proc.getActiveSlot() == 0 ? "COPY A>B" : "COPY B>A");
    if (proc.isLearning())
    {
        const float s = proc.learnSuggestionDb();
        learn.setButtonText ("APPLY");
        learnStatus = std::isfinite (s) ? "Measured. APPLY sets LEVEL to " + juce::String (s, 1) + " dB."
                                        : "Listening... play the vocal for at least 3 seconds.";
    }
    else
    {
        learn.setButtonText ("LEARN");
        learnStatus = "LEVEL " + juce::String (params.levelDb, 1) + " dB. Press LEARN while the vocal plays to match "
                      "the models to your recording level.";
    }
    content.repaint (bottomArea);
    content.repaint (deessArea.withHeight (40));
    content.repaint (heatArea.withHeight (40));
}

void VelvetEditor::showPresetMenu()
{
    juce::PopupMenu menu, user;
    const auto list = axv::factoryPresets();
    juce::String section;
    for (int i = 0; i < (int) list.size(); ++i)
    {
        if (section != list[(size_t) i].category)
        {
            section = list[(size_t) i].category;
            menu.addSectionHeader (section);
        }
        menu.addItem (1 + i, list[(size_t) i].name);
    }
    auto files = VelvetProcessor::userPresetFolder().findChildFiles (juce::File::findFiles, false, "*.axvpreset");
    files.sort();
    for (int i = 0; i < files.size(); ++i)
        user.addItem (1000 + i, files[i].getFileNameWithoutExtension());
    menu.addSeparator();
    menu.addSectionHeader ("USER");
    if (files.isEmpty())
        menu.addItem (-1, "(no user presets yet)", false);
    else
        menu.addSubMenu ("Your presets", user);
    juce::Component::SafePointer<VelvetEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetName), [safe, files] (int id)
    {
        if (safe == nullptr || id <= 0)
            return;
        if (id >= 1000)
            safe->proc.loadUserPreset (files[id - 1000]);
        else
            safe->proc.setCurrentProgram (id - 1);
    });
}

void VelvetEditor::promptSavePreset()
{
    using namespace juce;
    saveDialog = std::make_unique<AlertWindow> ("Save preset", "Name for this preset:", MessageBoxIconType::NoIcon, this);
    auto* w = saveDialog.get();
    w->setLookAndFeel (&lnf);
    w->addTextEditor ("name", proc.getPresetName(), {});
    w->addButton ("Save", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    Component::SafePointer<VelvetEditor> safe (this);
    w->enterModalState (true, ModalCallbackFunction::create ([safe] (int result)
    {
        if (safe == nullptr || safe->saveDialog == nullptr)
            return;
        const auto name = safe->saveDialog->getTextEditorContents ("name").trim();
        if (result == 1 && name.isNotEmpty() && ! safe->proc.saveUserPreset (name))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Save preset",
                                              "Could not write to " + VelvetProcessor::userPresetFolder().getFullPathName());
        MessageManager::callAsync ([safe] { if (safe != nullptr) safe->saveDialog.reset(); });
    }), false);
}
