#pragma once
// Axell Velvet - the whole signal chain, framework independent (no JUCE): the plugin wrapper only maps parameters.
//
//   in -> sanitize -> [dry ring: bypass, aligned to the full latency]
//      -> LOW CUT (24 dB/oct Butterworth: subsonic rumble would otherwise intermodulate with the voice in the models)
//      -> DE-ESS (sibilance detector + water-filling band gains, linear phase; latency DeEsser::latency())
//      -> HEAT: pre gain (LEVEL + DRIVE) -> upsample 2x..16x -> TAPE / CONSOLE / TUBE -> downsample -> post gain
//               (exact inverse, delayed by the oversampler) -> MIX with the de-essed signal (delayed alike), the heat
//               faded out on sibilants by S-GUARD -> TONE (high shelf)
//   S-GUARD blends the esses towards the clean signal instead of lowering the drive on them: the models have memory
//   (emphasis filters, transformer flux, tube bias, coupling capacitors), and a gain that swings 40 dB around them in a few
//   milliseconds turns that memory into thumps (measured, DECISIONS V8). The models always see a steady drive.
//      -> OUTPUT -> [bypass crossfade] -> structural fade -> out
//   meters: input loudness (LEARN), input / output peaks, the de-esser's detector and reduction, the added harmonics.
//
// Real-time safety: prepare() allocates everything; process() never allocates, locks or throws.
#include <axell/dsp/Filters.h>
#include <axell/dsp/History.h>
#include <axell/dsp/LatencyHandshake.h>
#include <axell/dsp/Loudness.h>
#include <axell/dsp/Oversampler.h>

#include <atomic>
#include <cstdint>

#include "Heat.h"
#include "Sibilance.h"

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define AXV_HAS_SSE 1
#endif

namespace axv
{
using namespace axell;

struct Params
{
    // DE-ESS
    bool deessOn = true;
    float amountPct = 50.0f;   // 0..100: sibilance may reach the voice body + (6 - 0.36 * amount) dB
    float rangeDb = 18.0f;     // maximum cut per band, 3..30 dB
    int focus = 0;             // 0 auto, 1 S (5.6 kHz up), 2 SH (below 5.6 kHz)
    bool listen = false;       // hear what the de-esser removes
    // HEAT
    bool heatOn = true;
    int mode = kTape;
    float drive = 4.0f;        // 0..10 (3.6 dB per step)
    float guardPct = 60.0f;    // S-GUARD: how much of the drive sibilants lose (100 % = esses are not saturated)
    float tone = 0.0f;         // -100 (dark) .. +100 (bright): high shelf -4 .. +4 dB at 4.5 kHz
    float mixPct = 100.0f;
    // GLOBAL
    float levelDb = 0.0f;      // operating level: the vocal's distance from the nominal -18 dBFS (LEARN sets it)
    float outputDb = 0.0f;
    int osLog2 = 3;            // HEAT oversampling 2x / 4x / 8x / 16x (log2 1..4)
    int lowCut = 1;            // input high-pass, 24 dB/oct: 0 off, 1 30 Hz (rumble only), 2 60 Hz, 3 90 Hz
    bool bypass = false;       // host bypass (latency-compensated)
};

struct DisplayFrame            // one per de-esser hop (2 ms)
{
    float p = 0.0f, hfReBodyDb = -60.0f, targetDb = 0.0f, grDb = 0.0f;
    float inPeakDb = -120.0f, outPeakDb = -120.0f;
    float guard = 1.0f;        // S-GUARD: share of the heat that reached the output (lowest in the hop)
};

struct ScopedFtz
{
#if AXV_HAS_SSE
    ScopedFtz() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }
    ~ScopedFtz() { _mm_setcsr (saved); }
    unsigned int saved;
#endif
};

class Engine
{
public:
    static constexpr int kChunk = 256;
    static constexpr float kNominalOffsetDb = -21.0f; // the models' u = 1 sits 21 dB above the nominal vocal level
    // DRIVE (0..10, steps of 0.5) -> dB into each model, measured (tools/HeatCalibrate.cpp) so that every model adds the
    // same amount of harmonics to a vocal at the nominal level: -55 dB at DRIVE 0 rising 4 dB per step to -15 dB at 10
    // measured on Axell's dry vocal at LEVEL +15 (nominal), 40 s (tools/HeatCalibrate.cpp; DECISIONS V7)
    static constexpr float kDriveTable[kNumModes][21] = {
        { 5.85f, 8.18f, 10.01f, 11.42f, 12.64f, 13.74f, 14.78f, 15.77f, 16.76f, 17.74f, 18.73f, 19.73f, 20.75f, 21.80f, 22.88f, 24.00f, 25.17f, 26.41f, 27.71f, 29.10f, 30.63f },   // tape
        { -1.84f, 0.09f, 1.94f, 3.72f, 5.45f, 7.15f, 8.85f, 10.59f, 12.37f, 14.24f, 16.18f, 18.20f, 20.25f, 22.27f, 24.24f, 26.13f, 27.82f, 29.24f, 30.53f, 31.78f, 33.07f }, // console
        { -9.64f, -7.55f, -5.50f, -3.47f, -1.46f, 0.54f, 2.53f, 4.52f, 6.51f, 8.48f, 10.45f, 12.41f, 14.35f, 16.27f, 18.15f, 19.96f, 21.60f, 23.01f, 24.24f, 25.41f, 26.60f } }; // tube
    static float driveToDb (int mode, float drive) noexcept
    {
        const float x = std::clamp (drive, 0.0f, 10.0f) * 2.0f;
        const int i = std::min ((int) x, 19);
        const float t = x - (float) i;
        const auto& tb = kDriveTable[std::clamp (mode, 0, kNumModes - 1)];
        return tb[i] + t * (tb[i + 1] - tb[i]);
    }

    void prepare (double sampleRate, int numChannels, int maxBlockSize = 0)
    {
        fs = sampleRate;
        numCh = std::clamp (numChannels, 1, kMaxCh);
        maxBlock = std::max (0, maxBlockSize);
        deess.prepare (fs, numCh);
        os.prepare (kChunk, fs, false);
        int maxOs = 0;
        for (int k = 1; k <= Oversampler::kMaxFactorLog2; ++k)
            maxOs = std::max (maxOs, os.latencyFor (k));
        const int maxLat = deess.latency() + maxOs;
        int ring = 1;
        while (ring < maxLat + kChunk + 1)
            ring <<= 1;
        ringMask = ring - 1;
        for (auto& r : dryRing)
            r.assign ((size_t) ring, 0.0f);
        for (auto& r : midRing)
            r.assign ((size_t) ring, 0.0f);
        gainRing.assign ((size_t) ring, 1.0f);
        guardRing.assign ((size_t) ring, 1.0f);
        inMeter.prepare (fs, numCh);
        headBump.setPeak (fs, 70.0, 1.0, 1.5);
        smoothK = 1.0f - onePole (0.03f, fs);
        switchK = 1.0f - onePole (0.01f, fs);
        fadeStep = (float) (1.0 / std::max (1.0, 0.003 * fs));
        fadeStepLc = (float) (1.0 / std::max (1.0, 0.01 * fs));
        setParams (params);
        active = want;
        applyConfig();
        hs.prepared (latency, fs);
        reset();
    }

    void reset() noexcept
    {
        const bool apply = ! hs.tracking() || latencyOf (want) == latencyOf (active);
        if (apply && want != active)
        {
            active = want;
            applyConfig();
        }
        deess.reset();
        os.reset();
        resetModels();
        for (auto& r : dryRing)
            std::fill (r.begin(), r.end(), 0.0f);
        for (auto& r : midRing)
            std::fill (r.begin(), r.end(), 0.0f);
        std::fill (gainRing.begin(), gainRing.end(), 1.0f);
        std::fill (guardRing.begin(), guardRing.end(), 1.0f);
        writePos = 0;
        driveDbNow = driveDbTarget;
        levelDbNow = levelDbTarget;
        heatRunning = false;
        outGain = outTarget;
        mixAmt = mixTarget;
        heatAmt = heatTarget;
        bypassAmt = bypassTarget;
        toneDbNow = toneDbTarget;
        tone.reset();
        toneSet = false;
        lowCut = lowCutWant;
        designLowCut();
        for (auto& f : hp)
            f.resetState();
        lowCutFade = 0.0f;
        fadeGain = fadeTgt = (want == active) ? 1.0f : 0.0f;
        warm = 0;
        hopIn = hopOut = 0.0f;
        hopGuard = 1.0f;
    }

    void setParams (const Params& p) noexcept
    {
        params = p;
        DeEssParams d;
        d.amountPct = p.deessOn ? std::clamp (p.amountPct, 0.0f, 100.0f) : 0.0f;
        d.rangeDb = std::clamp (p.rangeDb, 3.0f, 30.0f);
        d.focus = std::clamp (p.focus, 0, 2);
        d.listen = p.listen && p.deessOn;
        deess.setParams (d);
        wantMode = std::clamp (p.mode, 0, kNumModes - 1);
        driveDbTarget = driveToDb (wantMode, p.drive);
        levelDbTarget = std::clamp (p.levelDb, -24.0f, 36.0f);
        guard = std::clamp (p.guardPct, 0.0f, 100.0f) * 0.01f;
        mixTarget = std::clamp (p.mixPct, 0.0f, 100.0f) * 0.01f;
        heatTarget = p.heatOn && ! d.listen ? 1.0f : 0.0f;
        toneDbTarget = std::clamp (p.tone, -100.0f, 100.0f) * 0.04f;
        outTarget = dbToGain (std::clamp (p.outputDb, -18.0f, 18.0f));
        bypassTarget = p.bypass ? 1.0f : 0.0f;
        lowCutWant = std::clamp (p.lowCut, 0, 3);
        want.osLog2 = std::clamp (p.osLog2, 1, Oversampler::kMaxFactorLog2);
    }

    struct Config
    {
        int osLog2 = 3;
        bool operator== (const Config& o) const noexcept { return osLog2 == o.osLog2; }
        bool operator!= (const Config& o) const noexcept { return ! (*this == o); }
    };

    int latencyOf (const Config& c) const noexcept { return deess.latency() + os.latencyFor (c.osLog2); }
    int getLatencySamples() const noexcept { return latencyPub.load (std::memory_order_acquire); }
    LatencyHandshake& handshake() noexcept { return hs; }
    const LoudnessMeter& inputLoudness() const noexcept { return inMeter; }
    double measuredSeconds() const noexcept { return (double) measured.load (std::memory_order_relaxed) / fs; }
    void requestMeterReset() noexcept { meterResetReq.store (true, std::memory_order_release); }
    bool popDisplay (DisplayFrame& f) noexcept { return display.pop (f); }
    int activeMode() const noexcept { return mode; }
    double sampleRate() const noexcept { return fs; }

    void process (float* const* io, int numChannels, int numSamples) noexcept
    {
        ScopedFtz ftz;
        const int nCh = std::clamp (numChannels, 0, numCh);
        if (nCh == 0)
            return;
        if (meterResetReq.exchange (false, std::memory_order_acq_rel))
        {
            inMeter.reset();
            measured.store (0, std::memory_order_relaxed);
        }
        for (int off = 0; off < numSamples; off += kChunk)
        {
            const int n = std::min (kChunk, numSamples - off);
            float* p[kMaxCh] {};
            for (int c = 0; c < nCh; ++c)
                p[c] = io[c] + off;
            chunk (p, nCh, n);
        }
    }

private:
    void resetModels() noexcept
    {
        headBump.resetState();
        tape.reset();
        console.reset();
        tubeM.reset();
    }

    void applyConfig() noexcept
    {
        os.reset();
        const double fo = fs * (1 << active.osLog2);
        tape.prepare (fo);
        console.prepare (fo);
        tubeM.prepare (fo);
        latency = latencyOf (active);
        latencyPub.store (latency, std::memory_order_release);
    }

    void control (int n) noexcept
    {
        const bool ready = hs.ready (latencyOf (hs.isHolding() ? active : want), fadeGain <= 0.0f, n);
        const Config tgt = hs.isHolding() ? active : want;
        if (tgt != active)
        {
            fadeTgt = 0.0f;
            if (fadeGain <= 0.0f && ready)
            {
                active = tgt;
                applyConfig();
                warm = latency + (hs.tracking() ? maxBlock : 0);
            }
        }
        else
            fadeTgt = (! ready || warm > 0) ? 0.0f : 1.0f;
        if (warm > 0)
            warm = std::max (0, warm - n);
    }

    // one-pole glide that lands exactly on its target: near the end a float step (distance x k) falls below half an
    // ulp and the plain recursion stalls short of it (a "bypassed" output kept 0.0015 % of the wet signal)
    static void glide (float& v, float target, float k) noexcept
    {
        v += (target - v) * k;
        if (std::abs (target - v) <= 1.0e-4f * std::max (1.0f, std::abs (target)))
            v = target;
    }

    void runModel (float* x, int n, int ch) noexcept
    {
        switch (mode)
        {
            case kConsole: console.process (x, n, ch); break;
            case kTube: tubeM.process (x, n, ch); break;
            default: tape.process (x, n, ch); break;
        }
    }

    void chunk (float* const* io, int nCh, int n) noexcept
    {
        // 0. sanitize, dry ring, input loudness (LEARN)
        for (int c = 0; c < nCh; ++c)
            for (int i = 0; i < n; ++i)
            {
                float v = io[c][i];
                v = std::isfinite (v) ? std::clamp (v, -1000.0f, 1000.0f) : 0.0f;
                io[c][i] = v;
                dryRing[(size_t) c][(size_t) ((writePos + i) & ringMask)] = v;
                hopIn = std::max (hopIn, std::abs (v));
            }
        inMeter.process (io, nCh, n);
        control (n);

        // LOW CUT: switching re-tunes the filter and crossfades over 10 ms from the old filter's output (both run)
        if (lowCutWant != lowCut)
        {
            lowCutPrev = lowCut;
            lowCut = lowCutWant;
            for (int s = 0; s < 2; ++s)
                hpPrev[(size_t) s] = hp[(size_t) s];
            designLowCut();
            lowCutFade = 1.0f;
        }
        if (lowCut > 0 || lowCutFade > 0.0f)
            for (int c = 0; c < nCh; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const double x = io[c][i];
                    const double y = lowCut > 0 ? hp[1].process (hp[0].process (x, c), c) : x;
                    float out = (float) y;
                    if (lowCutFade > 0.0f)
                    {
                        const double yo = lowCutPrev > 0 ? hpPrev[1].process (hpPrev[0].process (x, c), c) : x;
                        const float f = std::max (0.0f, lowCutFade - (float) i * fadeStepLc);
                        out = (float) (y + (yo - y) * f);
                    }
                    io[c][i] = out;
                }
        if (lowCutFade > 0.0f)
            lowCutFade = std::max (0.0f, lowCutFade - (float) n * fadeStepLc);

        // 1. de-esser (always running: constant latency; amount 0 = an exact delay)
        deess.process (io, nCh, n, sib.data());

        // 2. HEAT. A mode change crossfades to the de-essed signal and back (the model starts from rest).
        const bool modeSwitch = wantMode != mode;
        const float heatGoal = modeSwitch ? 0.0f : heatTarget;
        const int log2 = active.osLog2, factor = 1 << log2, Los = os.latencyFor (log2);
        const bool runHeat = heatAmt > 0.0f || heatGoal > 0.0f;
        // pre gain per sample (dB smoothed; S-GUARD lowers the drive on sibilants), its inverse delayed by Los
        if (runHeat && ! heatRunning)
        {
            os.reset(); // starting from off: the oversampler and the model start from rest (the mix fades in)
            resetModels();
        }
        heatRunning = runHeat;
        for (int i = 0; i < n; ++i)
        {
            glide (driveDbNow, driveDbTarget, smoothK);
            glide (levelDbNow, levelDbTarget, smoothK);
            pre[(size_t) i] = dbToGain (levelDbNow + kNominalOffsetDb + driveDbNow);
            gainRing[(size_t) ((writePos + i) & ringMask)] = pre[(size_t) i];
            // S-GUARD weight, aligned with the oversampler's output (the sibilance of the sample coming out now)
            guardRing[(size_t) ((writePos + i) & ringMask)] = 1.0f - guard * sib[(size_t) i];
        }
        for (int c = 0; c < nCh; ++c)
        {
            float* x = io[c];
            auto& mid = midRing[(size_t) c];
            for (int i = 0; i < n; ++i)
                mid[(size_t) ((writePos + i) & ringMask)] = x[i];
            if (runHeat)
            {
                for (int i = 0; i < n; ++i)
                    wetIn[(size_t) i] = x[i] * pre[(size_t) i];
                float* hi = os.up (wetIn.data(), n, c, log2);
                runModel (hi, n * factor, c);
                os.down (wet.data(), n, c, log2);
                // TAPE playback head bump (linear, after the model): the head's contact length (~5 mm) resonates with
                // the 15 ips tape speed near v / L = 76 Hz; +1.5 dB at 70 Hz, Q 1 (DECISIONS V15)
                if (mode == kTape)
                    for (int i = 0; i < n; ++i)
                        wet[(size_t) i] = (float) headBump.process (wet[(size_t) i], c);
            }
            float h = heatAmt, m = mixAmt;
            for (int i = 0; i < n; ++i)
            {
                const int at = (writePos + i - Los) & ringMask;
                const float dry = mid[(size_t) at];
                float y = dry;
                if (runHeat)
                {
                    const float w = wet[(size_t) i] / gainRing[(size_t) at];
                    const float e = w - dry;
                    glide (h, heatGoal, switchK);
                    glide (m, mixTarget, switchK);
                    const float g = guardRing[(size_t) at];
                    y = dry + e * (h * m * g);
                    hopGuard = std::min (hopGuard, g);
                }
                x[i] = y;
            }
            if (c == nCh - 1)
            {
                heatAmt = runHeat ? h : heatGoal;
                mixAmt = runHeat ? m : mixTarget;
            }
        }
        if (modeSwitch && heatAmt <= 0.0f)
        {
            mode = wantMode;
            resetModels();
            os.reset();
        }

        // 3. TONE (follows the HEAT switch), OUTPUT, bypass, structural fade
        const float toneGoal = toneDbTarget * heatAmt;
        toneDbNow += (toneGoal - toneDbNow) * std::min (1.0f, smoothK * (float) n);
        if (! toneSet || std::abs (toneDbNow - toneApplied) > 1.0e-4f)
        {
            toneShelf (toneDbNow);
            toneApplied = toneDbNow;
            toneSet = true;
        }
        const bool toneActive = std::abs (toneApplied) > 1.0e-4f;
        const int L = latency;
        for (int i = 0; i < n; ++i)
        {
            const int at = (writePos + i - L) & ringMask;
            glide (outGain, outTarget, smoothK);
            glide (bypassAmt, bypassTarget, switchK);
            fadeGain = fadeTgt > fadeGain ? std::min (fadeTgt, fadeGain + fadeStep) : std::max (fadeTgt, fadeGain - fadeStep);
            for (int c = 0; c < nCh; ++c)
            {
                float y = io[c][i];
                if (toneActive)
                    y = (float) tone.process (y, c);
                y *= outGain;
                if (bypassAmt >= 1.0f)
                    y = dryRing[(size_t) c][(size_t) at]; // fully bypassed: the dry signal exactly
                else if (bypassAmt > 0.0f)
                    y += (dryRing[(size_t) c][(size_t) at] - y) * bypassAmt;
                y *= fadeGain;
                io[c][i] = y;
                hopOut = std::max (hopOut, std::abs (y));
            }
            if (++hopCount >= deess.detector().hopSize())
                pushDisplay();
        }
        measured.store (measured.load (std::memory_order_relaxed) + n, std::memory_order_relaxed);
        writePos = (writePos + n) & ringMask;
    }

    void pushDisplay() noexcept
    {
        auto db = [] (float a) { return a > 1.0e-6f ? 20.0f * std::log10 (a) : -120.0f; };
        const auto& d = deess.display();
        DisplayFrame f;
        f.p = d.p;
        f.hfReBodyDb = d.hfReBodyDb;
        f.targetDb = d.targetDb;
        f.grDb = d.grDb;
        f.inPeakDb = db (hopIn);
        f.outPeakDb = db (hopOut);
        f.guard = hopGuard;
        display.push (f);
        hopCount = 0;
        hopIn = hopOut = 0.0f;
        hopGuard = 1.0f;
    }

    // 4th-order Butterworth high-pass (two biquads, Q 0.541 / 1.307)
    void designLowCut() noexcept
    {
        static constexpr double kHz[4] = { 0.0, 30.0, 60.0, 90.0 };
        if (lowCut > 0)
        {
            hp[0].setHighPass (fs, kHz[lowCut], 0.54119610);
            hp[1].setHighPass (fs, kHz[lowCut], 1.30656296);
        }
    }

    // first-order high shelf at 4.5 kHz (bilinear, prewarped)
    void toneShelf (float gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 20.0), w = std::tan (kPi * 4500.0 / fs);
        // H(s) = (A s + w) / (s + w) with the corner at sqrt(A) for a symmetric shelf
        const double wc = w * std::sqrt (A);
        const double b0 = (A + wc) / (1.0 + wc), b1 = (wc - A) / (1.0 + wc), a1 = (wc - 1.0) / (1.0 + wc);
        tone.set (b0, b1, a1);
    }

    struct FirstOrder
    {
        double b0 = 1, b1 = 0, a1 = 0;
        std::array<double, kMaxCh> z {};
        void set (double nb0, double nb1, double na1) noexcept { b0 = nb0; b1 = nb1; a1 = na1; }
        double process (double x, int ch) noexcept
        {
            const double y = b0 * x + z[(size_t) ch];
            z[(size_t) ch] = b1 * x - a1 * y;
            return y;
        }
        void reset() noexcept { z.fill (0.0); }
    };

    double fs = 48000.0;
    int numCh = 2, maxBlock = 0, latency = 0, warm = 0, ringMask = 0, writePos = 0, hopCount = 0;
    int mode = kTape, wantMode = kTape;
    Params params;
    Config active, want;
    DeEsser deess;
    Oversampler os;
    TapeModel tape;
    ConsoleModel console;
    TubeModel tubeM;
    FirstOrder tone;
    std::array<axell::Biquad, 2> hp, hpPrev;
    axell::Biquad headBump;
    int lowCut = 1, lowCutWant = 1, lowCutPrev = 0;
    float lowCutFade = 0.0f, fadeStepLc = 0.001f;
    LoudnessMeter inMeter;
    LatencyHandshake hs;
    HistoryFifo<DisplayFrame, 4096> display;
    std::atomic<int> latencyPub { 0 };
    std::atomic<bool> meterResetReq { false };
    std::atomic<int64_t> measured { 0 };
    std::array<std::vector<float>, kMaxCh> dryRing, midRing;
    std::vector<float> gainRing, guardRing;
    std::array<float, kChunk> sib {}, pre {}, wetIn {}, wet {};
    float driveDbNow = 0.0f, driveDbTarget = 0.0f, levelDbNow = 0.0f, levelDbTarget = 0.0f, guard = 0.6f;
    bool heatRunning = false;
    float mixAmt = 1.0f, mixTarget = 1.0f, heatAmt = 1.0f, heatTarget = 1.0f;
    float toneDbNow = 0.0f, toneDbTarget = 0.0f, toneApplied = 0.0f;
    bool toneSet = false;
    float outGain = 1.0f, outTarget = 1.0f, bypassAmt = 0.0f, bypassTarget = 0.0f;
    float smoothK = 0.001f, switchK = 0.002f, fadeGain = 1.0f, fadeTgt = 1.0f, fadeStep = 0.01f;
    float hopIn = 0.0f, hopOut = 0.0f, hopGuard = 1.0f;
};
} // namespace axv
