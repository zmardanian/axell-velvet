// Axell Velvet - DSP suite (JUCE-free: the Source/ headers exactly as they ship). Exit code = failed checks.
//   VelvetDspTests          44.1 / 48 / 96 kHz + CPU benchmark
//   VelvetDspTests --quick  48 kHz only, no CPU benchmark
#include "Engine.h"
#include "Presets.h"

#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

using namespace axv;
static int fails = 0, checks = 0;
static thread_local bool countAllocs = false;
static long allocCount = 0;
void* operator new (std::size_t n)
{
    if (countAllocs)
        ++allocCount;
    if (void* p = std::malloc (n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

static void check (const std::string& name, double v, double lo, double hi, const char* unit)
{
    ++checks;
    const bool ok = v >= lo && v <= hi;
    fails += ok ? 0 : 1;
    std::printf ("%s  %-100s %11.4f %s  [%g .. %g]\n", ok ? "PASS" : "FAIL", name.c_str(), v, unit, lo, hi);
}
static double db (double a) { return 20.0 * std::log10 (std::max (a, 1e-30)); }
static std::string rateName (double fs) { char b[32]; std::snprintf (b, sizeof b, "%.1f kHz", fs / 1000.0); return b; }

using Buf = std::vector<std::vector<float>>;
static Buf run (Engine& e, Buf x, int block = 512, bool count = false)
{
    const int n = (int) x[0].size(), nc = (int) x.size();
    for (int p = 0; p < n; p += block)
    {
        float* io[2] {};
        for (int c = 0; c < nc; ++c)
            io[c] = x[(size_t) c].data() + p;
        countAllocs = count;
        e.process (io, nc, std::min (block, n - p));
        countAllocs = false;
    }
    return x;
}
static std::unique_ptr<Engine> make (double fs, const Params& p, int nc = 2, int maxBlock = 512)
{
    auto e = std::make_unique<Engine>();
    e->setParams (p);
    e->prepare (fs, nc, maxBlock);
    return e;
}
// render with the latency removed (input padded with zeros at the end)
static Buf render (double fs, const Params& p, Buf x, int block = 512)
{
    auto e = make (fs, p, (int) x.size(), std::max (block, 512));
    const int L = e->getLatencySamples();
    for (auto& c : x)
        c.resize (c.size() + (size_t) L, 0.0f);
    auto y = run (*e, x, block);
    for (auto& c : y)
        c.erase (c.begin(), c.begin() + L);
    return y;
}
static Params clean() // de-esser at 0 %, heat off, low cut off: an exact delay
{
    Params p;
    p.amountPct = 0.0f;
    p.heatOn = false;
    p.lowCut = 0;
    return p;
}

// synthetic voice: vowels (pulse train through formants, f0 120-160 Hz) alternating with sibilants - "s" (noise peaking
// at 7.5 kHz) and "sh" (noise around 3.4 kHz) - and silences; markers of where each is
struct Voice
{
    Buf x;
    std::vector<std::pair<size_t, size_t>> vowels, esses;
};
static Voice voice (double fs, double secs, double gainDb = -14.0, unsigned seed = 5)
{
    Voice v;
    const size_t n = (size_t) (secs * fs);
    v.x.assign (2, std::vector<float> (n, 0.0f));
    std::mt19937 g (seed);
    std::normal_distribution<double> nd;
    const double gain = std::pow (10.0, gainDb / 20.0);
    double ph = 0.0;
    // two-pole resonators for the sibilant noise
    // two-pole resonators specified by bandwidth in Hz (the same sound at every sample rate); the noise is band-limited
    // to 16 kHz first (a 4-pole low-pass), as a microphone and a 32-48 kHz recording would
    auto reson = [&] (double f, double bw) { const double w = 2 * kPi * f / fs, r = std::exp (-kPi * bw / fs); return std::array<double, 3> { 2 * r * std::cos (w), -r * r, (1 - r) }; };
    const auto rs = reson (7500, 1500), rsh = reson (3400, 1200);
    axell::Biquad nlp1, nlp2;
    nlp1.setLowPass (fs, 16000.0);
    nlp2.setLowPass (fs, 16000.0);
    double s1 = 0, s2 = 0, h1 = 0, h2 = 0;
    const double seg = 0.42; // vowel 0.30 s, sibilant 0.12 s, then repeat; a 0.25 s gap every 4th
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        const int k = (int) (t / seg);
        const double u = t - k * seg;
        const bool gap = k % 4 == 3 && u < 0.25;
        const bool vowel = ! gap && u < 0.30;
        const bool sib = ! gap && u >= 0.30;
        const bool sh = k % 2 == 1;
        double y = 0.0;
        if (vowel)
        {
            const double f0 = 140.0 + 20.0 * std::sin (2 * kPi * 0.7 * t);
            ph += 2 * kPi * f0 / fs;
            const double env = std::min (1.0, std::min (u, 0.30 - u) / 0.02);
            double s = 0;
            for (int h = 1; h * f0 < std::min (0.45 * fs, 9000.0); ++h)
            {
                const double f = h * f0;
                const double a = std::exp (-std::pow ((f - 650) / 250, 2)) + 0.6 * std::exp (-std::pow ((f - 1150) / 300, 2))
                                 + 0.2 * std::exp (-std::pow ((f - 2600) / 500, 2)) + 0.05 * std::exp (-std::pow ((f - 3500) / 600, 2));
                s += a * std::sin (h * ph) / std::sqrt ((double) h);
            }
            y = 0.25 * env * s;
        }
        const double w = nlp2.process (nlp1.process (nd (g), 0), 0);
        const double a = rs[2] * w + rs[0] * s1 + rs[1] * s2;
        s2 = s1;
        s1 = a;
        const double b = rsh[2] * w + rsh[0] * h1 + rsh[1] * h2;
        h2 = h1;
        h1 = b;
        if (sib)
        {
            const double uu = u - 0.30, env = std::sin (kPi * uu / 0.12);
            y += env * (sh ? 0.9 * b : 0.6 * a);
        }
        v.x[0][i] = v.x[1][i] = (float) (gain * y);
    }
    for (double t0 = 0.0; t0 + seg <= secs; t0 += seg)
    {
        const int k = (int) std::lround (t0 / seg);
        if (k % 4 == 3)
            continue;
        v.vowels.push_back ({ (size_t) ((t0 + 0.06) * fs), (size_t) ((t0 + 0.26) * fs) });
        v.esses.push_back ({ (size_t) ((t0 + 0.33) * fs), (size_t) ((t0 + 0.39) * fs) });
    }
    return v;
}

static Buf sine (double fs, double f, double ampDb, double secs, int nc = 2)
{
    Buf b ((size_t) nc, std::vector<float> ((size_t) (secs * fs)));
    const double a = std::pow (10.0, ampDb / 20.0);
    for (size_t i = 0; i < b[0].size(); ++i)
        for (auto& c : b)
            c[i] = (float) (a * std::sin (2 * kPi * f * (double) i / fs));
    return b;
}

// amplitude at frequency f over [from, from + len) (Blackman window, exact for harmonics of 1/len)
static double toneAmp (const std::vector<float>& x, size_t from, size_t len, double f, double fs)
{
    std::complex<double> s = 0;
    double ws = 0;
    for (size_t i = 0; i < len; ++i)
    {
        const double w = 0.42 - 0.5 * std::cos (2 * kPi * (double) i / (double) len) + 0.08 * std::cos (4 * kPi * (double) i / (double) len);
        s += w * (double) x[from + i] * std::polar (1.0, -2 * kPi * f * (double) i / fs);
        ws += w;
    }
    return 2.0 * std::abs (s) / ws;
}

// worst non-harmonic component 20 Hz .. min(20 kHz, 0.45 fs) relative to the fundamental (Blackman, FFT-free scan of
// every bin of a 1 s window; harmonics +-12 Hz excluded)
static double worstF = 0.0;
static double worstAlias (const std::vector<float>& y, size_t from, double fs, double f0)
{
    const size_t N = (size_t) fs;
    std::vector<double> w (N);
    for (size_t i = 0; i < N; ++i)
    {
        const double t = 2 * kPi * (double) i / (double) N; // 4-term Blackman-Harris: sidelobes -92 dB
        w[i] = (0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t)) * y[from + i];
    }
    axell::RealFft fft;
    size_t M = 1;
    while (M < N)
        M <<= 1;
    std::vector<float> in (M, 0.0f), out (M + 2);
    for (size_t i = 0; i < N; ++i)
        in[i] = (float) w[i];
    fft.prepare ((int) M);
    fft.forward (in.data(), out.data());
    double fund = 0, worst = 0;
    const double top = std::min (20000.0, 0.45 * fs);
    for (size_t k = 1; k <= M / 2; ++k)
    {
        const double f = (double) k * fs / (double) M;
        if (f < 20.0 || f > top)
            continue;
        const double m = std::hypot ((double) out[2 * k], (double) out[2 * k + 1]);
        bool harm = false;
        for (int h = 1; h * f0 < fs * 0.5 + 100; ++h)
            if (std::abs (f - h * f0) < 40.0)
                harm = true;
        if (std::abs (f - f0) < 40.0)
            fund = std::max (fund, m);
        else if (! harm && m > worst)
        {
            worst = m;
            worstF = f;
        }
    }
    return db (worst / fund);
}

static double bandEnergyDb (const std::vector<float>& x, const std::vector<std::pair<size_t, size_t>>& segs, double fs, double lo, double hi)
{
    // energy of x in [lo, hi) Hz over the segments: 4th-order band-pass by two RBJ biquads each side
    axell::Biquad hp1, hp2, lp1, lp2;
    hp1.setHighPass (fs, lo);
    hp2.setHighPass (fs, lo);
    lp1.setLowPass (fs, hi);
    lp2.setLowPass (fs, hi);
    std::vector<double> f (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        f[i] = lp2.process (lp1.process (hp2.process (hp1.process (x[i], 0), 0), 0), 0);
    double e = 0;
    for (auto [a, b] : segs)
        for (size_t i = a; i < b && i < f.size(); ++i)
            e += f[i] * f[i];
    return 10.0 * std::log10 (e + 1e-30);
}

//==============================================================================
static void testRate (double fs, bool quick)
{
    const std::string R = " @ " + rateName (fs);
    // 1. exact delay: de-esser 0 %, heat off, any block size
    {
        std::mt19937 g (1);
        std::normal_distribution<float> nd (0.0f, 0.2f);
        Buf x (2, std::vector<float> ((size_t) (fs * 1.5)));
        for (auto& c : x)
            for (auto& v : c)
                v = nd (g);
        for (int block : { 1, 37, 512, 4096 })
        {
            const auto y = render (fs, clean(), x, block);
            double err = 0;
            for (int c = 0; c < 2; ++c)
                for (size_t i = 0; i < x[0].size(); ++i)
                    err = std::max (err, (double) std::abs (y[(size_t) c][i] - x[(size_t) c][i]));
            check ("exact delay (de-ess 0 %, heat off), block " + std::to_string (block) + ": max |out - in|" + R, err, 0.0, 0.0, "");
        }
    }
    // 2. latency: an impulse through heat on (drive 0) and the de-esser comes out at exactly the reported latency
    for (int mode = 0; mode < kNumModes; ++mode)
        for (int os : { 1, 4 })
        {
            Params p;
            p.mode = mode;
            p.drive = 0.0f;
            p.osLog2 = os;
            p.levelDb = -24.0f;
            auto e = make (fs, p);
            const int L = e->getLatencySamples();
            Buf x (2, std::vector<float> ((size_t) (fs * 0.5), 0.0f));
            const size_t at = (size_t) (0.2 * fs);
            x[0][at] = x[1][at] = 0.5f;
            const auto y = run (*e, x);
            size_t pk = 0;
            for (size_t i = 0; i < y[0].size(); ++i)
                if (std::abs (y[0][i]) > std::abs (y[0][pk]))
                    pk = i;
            check (std::string ("impulse peak - reported latency, ") + (mode == 0 ? "tape" : mode == 1 ? "console" : "tube") + " "
                       + std::to_string (1 << os) + "x" + R, (double) pk - (double) at - L, 0.0, 0.0, "smp");
        }
    // 3. silence in -> exactly zero out (every mode, full drive, every oversampling)
    for (int mode = 0; mode < kNumModes; ++mode)
    {
        Params p;
        p.mode = mode;
        p.drive = 10.0f;
        p.levelDb = 36.0f;
        p.tone = 100.0f;
        auto y = render (fs, p, Buf (2, std::vector<float> ((size_t) fs, 0.0f)));
        double m = 0;
        for (auto& c : y)
            for (float v : c)
                m = std::max (m, (double) std::abs (v));
        check (std::string ("silence in, full drive: max |out|, mode ") + std::to_string (mode) + R, m, 0.0, 0.0, "");
    }
    // 4. NaN / Inf in the input: the output stays finite and recovers
    {
        auto v = voice (fs, 3.0);
        v.x[0][(size_t) fs / 2] = std::numeric_limits<float>::quiet_NaN();
        v.x[1][(size_t) fs / 2 + 10] = std::numeric_limits<float>::infinity();
        Params p;
        p.drive = 8.0f;
        auto y = render (fs, p, v.x);
        double bad = 0, late = 0;
        for (auto& c : y)
            for (size_t i = 0; i < c.size(); ++i)
            {
                if (! std::isfinite (c[i]))
                    bad += 1;
                if (i > (size_t) (1.5 * fs))
                    late = std::max (late, (double) std::abs (c[i]));
            }
        check ("NaN / Inf input: non-finite output samples" + R, bad, 0, 0, "");
        check ("NaN / Inf input: output alive afterwards (peak)" + R, db (late), -40.0, 0.0, "dBFS");
    }
    // 5. block-size independence (defaults, the synthetic voice)
    {
        auto v = voice (fs, 2.5);
        Params p;
        const auto ref = render (fs, p, v.x, 512);
        for (int block : { 1, 64, 4096 })
        {
            const auto y = render (fs, p, v.x, block);
            double d = 0;
            for (int c = 0; c < 2; ++c)
                for (size_t i = 0; i < y[0].size(); ++i)
                    d = std::max (d, (double) std::abs (y[(size_t) c][i] - ref[(size_t) c][i]));
            check ("block size " + std::to_string (block) + " vs 512: max |difference|" + R, d, 0.0, 1.0e-6, "");
        }
    }
    // 6. HEAT: unity small-signal gain and flat response (tone 0), every mode
    for (int mode = 0; mode < kNumModes; ++mode)
    {
        Params p;
        p.amountPct = 0.0f;
        p.mode = mode;
        p.drive = 5.0f;
        double g1k = 0;
        for (double f : { 1000.0, 50.0, 70.0, 200.0, 5000.0, 10000.0, 16000.0 })
        {
            if (f > 0.4 * fs)
                continue;
            auto x = sine (fs, f, -60.0, 1.0);
            const auto y = render (fs, p, x);
            const size_t len = (size_t) (0.5 * fs);
            const double g = db (toneAmp (y[0], (size_t) (0.4 * fs), len, f, fs) / toneAmp (x[0], (size_t) (0.4 * fs), len, f, fs));
            const std::string mn = mode == 0 ? "tape" : mode == 1 ? "console" : "tube";
            if (f == 1000.0)
            {
                g1k = g;
                check ("HEAT " + mn + " drive 5: small-signal gain at 1 kHz (auto gain)" + R, g, -0.25, 0.25, "dB");
            }
            else if (mode == kTape && f < 150.0)
                check ("HEAT " + mn + " drive 5: response at " + std::to_string ((int) f) + " Hz re 1 kHz (head bump)" + R, g - g1k,
                       0.8, 1.6, "dB");
            else
                check ("HEAT " + mn + " drive 5: response at " + std::to_string ((int) f) + " Hz re 1 kHz" + R, g - g1k,
                       f >= 10000.0 ? -1.3 : -0.5, 0.5, "dB");
        }
    }
    // 7. HEAT: harmonics grow with DRIVE (1 kHz at -15 dBFS = a vocal peak at nominal level), every mode
    for (int mode = 0; mode < kNumModes; ++mode)
    {
        double prev = -300.0;
        bool mono = true;
        std::string row;
        for (float d : { 0.0f, 2.5f, 5.0f, 7.5f, 10.0f })
        {
            Params p;
            p.amountPct = 0.0f;
            p.mode = mode;
            p.drive = d;
            p.guardPct = 0.0f;
            auto x = sine (fs, 1000.0, -15.0, 1.0);
            const auto y = render (fs, p, x);
            const size_t from = (size_t) (0.4 * fs), len = (size_t) (0.5 * fs);
            const double h1 = toneAmp (y[0], from, len, 1000.0, fs);
            double hs = 0;
            for (int k = 2; k <= 9; ++k)
                if (k * 1000.0 < 0.45 * fs)
                    hs += std::pow (toneAmp (y[0], from, len, k * 1000.0, fs), 2);
            const double thd = 10.0 * std::log10 (hs / (h1 * h1) + 1e-30);
            mono = mono && thd > prev;
            prev = thd;
            char b[32];
            std::snprintf (b, sizeof b, " %.1f", thd);
            row += b;
        }
        std::printf ("INFO  HEAT mode %d THD (dB) at DRIVE 0 / 2.5 / 5 / 7.5 / 10:%s%s\n", mode, row.c_str(), R.c_str());
        check ("HEAT mode " + std::to_string (mode) + ": harmonics rise with every DRIVE step" + R, mono ? 1 : 0, 1, 1, "");
    }
    // 8. HEAT aliasing: 7 kHz at -6 dBFS, full DRIVE, LEVEL 0 (deep saturation), 16x and 8x
    for (int mode = 0; mode < kNumModes; ++mode)
        for (int os : { 3, 4 })
        {
            if (quick && os == 3)
                continue;
            Params p;
            p.amountPct = 0.0f;
            p.mode = mode;
            p.drive = 10.0f;
            p.guardPct = 0.0f;
            p.osLog2 = os;
            const double f0 = 6997.0;
            auto x = sine (fs, f0, -6.0, 1.6, 1);
            const auto y = render (fs, p, x);
            const double a = worstAlias (y[0], (size_t) (0.5 * fs), fs, f0);
            check ("HEAT mode " + std::to_string (mode) + " " + std::to_string (1 << os) + "x: worst alias (at " + std::to_string ((int) worstF) + " Hz), 7 kHz -6 dBFS full drive" + R, a,
                   -300.0, os == 4 ? -95.0 : -85.0, "dBc");
        }
    // 9. DE-ESS on the synthetic voice: esses down, vowels untouched
    {
        auto v = voice (fs, 6.0);
        Params p;
        p.heatOn = false;
        p.amountPct = 75.0f;
        p.lowCut = 0; // so that output + removed == the raw input
        const auto y = render (fs, p, v.x);
        // skip the first 1.3 s (the voice reference and the detector's adaptation settle)
        std::vector<std::pair<size_t, size_t>> es, vs;
        for (auto s : v.esses)
            if (s.first > (size_t) (1.3 * fs)) es.push_back (s);
        for (auto s : v.vowels)
            if (s.first > (size_t) (1.3 * fs)) vs.push_back (s);
        const double eIn = bandEnergyDb (v.x[0], es, fs, 2500.0, 12000.0), eOut = bandEnergyDb (y[0], es, fs, 2500.0, 12000.0);
        check ("DE-ESS 75 %: sibilant 2.5-12 kHz energy reduced by" + R, eIn - eOut, 6.0, 40.0, "dB");
        double vmax = -300.0;
        for (auto s : vs)
        {
            double a = 0, b = 0;
            for (size_t i = s.first; i < s.second; ++i)
            {
                a += (double) v.x[0][i] * v.x[0][i];
                b += (double) (y[0][i] - v.x[0][i]) * (y[0][i] - v.x[0][i]);
            }
            vmax = std::max (vmax, 10.0 * std::log10 (b / a + 1e-30));
        }
        check ("DE-ESS 75 %: worst change on a vowel (difference re vowel)" + R, vmax, -300.0, -60.0, "dB");
        // listen: what is removed + what remains == the input (exactly the de-esser's two outputs)
        Params pl = p;
        pl.listen = true;
        const auto rem = render (fs, pl, v.x);
        double err = 0;
        for (size_t i = 0; i < y[0].size(); ++i)
            err = std::max (err, (double) std::abs (y[0][i] + rem[0][i] - v.x[0][i]));
        check ("DE-ESS LISTEN: output + removed == input" + R, err, 0.0, 1.0e-6, "");
    }
    // 10. level independence: the same voice 30 dB quieter is de-essed by the same amount
    {
        Params p;
        p.heatOn = false;
        p.amountPct = 75.0f;
        auto a = voice (fs, 6.0, -14.0), b = voice (fs, 6.0, -44.0);
        const auto ya = render (fs, p, a.x), yb = render (fs, p, b.x);
        std::vector<std::pair<size_t, size_t>> es;
        for (auto s : a.esses)
            if (s.first > (size_t) (1.3 * fs)) es.push_back (s);
        const double ra = bandEnergyDb (a.x[0], es, fs, 2500.0, 12000.0) - bandEnergyDb (ya[0], es, fs, 2500.0, 12000.0);
        const double rb = bandEnergyDb (b.x[0], es, fs, 2500.0, 12000.0) - bandEnergyDb (yb[0], es, fs, 2500.0, 12000.0);
        check ("DE-ESS level independence: reduction at -44 dB minus at -14 dB" + R, rb - ra, -0.5, 0.5, "dB");
    }
    // 11. S-GUARD: the esses are driven less (difference energy on the esses, guard 100 % vs 0 %)
    {
        auto v = voice (fs, 4.0);
        Params p;
        p.amountPct = 0.0f;
        p.drive = 8.0f;
        p.guardPct = 0.0f;
        Params q = p;
        q.guardPct = 100.0f;
        Params lin = p; // the clean reference: HEAT off (what S-GUARD fades the esses back to)
        lin.heatOn = false;
        const auto y0 = render (fs, p, v.x), y1 = render (fs, q, v.x), yl = render (fs, lin, v.x);
        std::vector<std::pair<size_t, size_t>> es;
        for (auto s : v.esses)
            if (s.first > (size_t) (1.3 * fs)) es.push_back (s);
        double e0 = 0, e1 = 0;
        for (auto [a, b] : es)
            for (size_t i = a; i < b; ++i)
            {
                e0 += std::pow ((double) y0[0][i] - yl[0][i], 2);
                e1 += std::pow ((double) y1[0][i] - yl[0][i], 2);
            }
        check ("S-GUARD 100 % vs 0 %: heat added to the esses lower by" + R, 10.0 * std::log10 (e0 / (e1 + 1e-30)), 10.0, 200.0, "dB");
    }
    // 12. MIX 0 == heat off (exactly); OUTPUT +6.02 dB == x2
    {
        auto v = voice (fs, 2.0);
        Params a;
        a.mixPct = 0.0f;
        a.drive = 9.0f;
        Params b = a;
        b.heatOn = false;
        const auto ya = render (fs, a, v.x), yb = render (fs, b, v.x);
        double d = 0;
        for (size_t i = 0; i < ya[0].size(); ++i)
            d = std::max (d, (double) std::abs (ya[0][i] - yb[0][i]));
        check ("MIX 0 % equals HEAT off: max |difference|" + R, d, 0.0, 0.0, "");
        Params c = b;
        c.outputDb = 20.0f * std::log10 (2.0f);
        const auto yc = render (fs, c, v.x);
        double e = 0;
        for (size_t i = (size_t) (0.2 * fs); i < yc[0].size(); ++i)
            e = std::max (e, (double) std::abs (yc[0][i] - 2.0f * yb[0][i]));
        check ("OUTPUT +6.02 dB doubles the signal: max |error|" + R, e, 0.0, 1.0e-5, "");
    }
    // 13. mode switches while playing: no click (largest sample step <= 1.5 x the input's own largest step)
    {
        auto v = voice (fs, 4.0);
        Params p;
        p.drive = 7.0f;
        auto e = make (fs, p);
        Buf y = v.x;
        const int block = 256;
        float maxIn = 0.0f;
        for (size_t i = 1; i < v.x[0].size(); ++i)
            maxIn = std::max (maxIn, std::abs (v.x[0][i] - v.x[0][i - 1]));
        for (size_t s = 0, k = 0; s < y[0].size(); s += block, ++k)
        {
            if (k % 120 == 60)
            {
                p.mode = (p.mode + 1) % kNumModes;
                e->setParams (p);
            }
            float* io[2] = { y[0].data() + s, y[1].data() + s };
            e->process (io, 2, (int) std::min<size_t> (block, y[0].size() - s));
        }
        float maxOut = 0.0f;
        for (size_t i = 1; i < y[0].size(); ++i)
            maxOut = std::max (maxOut, std::abs (y[0][i] - y[0][i - 1]));
        check ("mode switches while playing: largest sample step re the input's" + R, maxOut / maxIn, 0.0, 1.5, "x");
    }
    // 14. oversampling change without a host (offline): applied at once, output aligned to the new latency
    {
        Params p;
        p.drive = 0.0f;
        p.levelDb = -24.0f;
        p.osLog2 = 2;
        auto e = make (fs, p);
        p.osLog2 = 4;
        e->setParams (p);
        Buf x (2, std::vector<float> ((size_t) fs, 0.0f));
        run (*e, Buf (2, std::vector<float> (2048, 0.0f)));
        const int L = e->getLatencySamples();
        x[0][(size_t) (0.5 * fs)] = 0.5f;
        const auto y = run (*e, x);
        size_t pk = 0;
        for (size_t i = 0; i < y[0].size(); ++i)
            if (std::abs (y[0][i]) > std::abs (y[0][pk]))
                pk = i;
        check ("oversampling 4x -> 16x offline: impulse at the new latency" + R, (double) pk - 0.5 * fs - L, 0.0, 0.0, "smp");
    }
    // 14b. LOW CUT: -3 dB at the corner, flat in the voice range, switching without clicks
    for (int lc : { 1, 2, 3 })
    {
        const double fc = lc == 1 ? 30.0 : lc == 2 ? 60.0 : 90.0;
        Params p = clean();
        p.lowCut = lc;
        auto amp = [&] (double f)
        {
            auto x = sine (fs, f, -20.0, 2.0, 1);
            const auto y = render (fs, p, x);
            const size_t len = (size_t) fs;
            return db (toneAmp (y[0], (size_t) (0.8 * fs), len, f, fs) / toneAmp (x[0], (size_t) (0.8 * fs), len, f, fs));
        };
        check ("LOW CUT " + std::to_string ((int) fc) + " Hz: gain at the corner" + R, amp (fc), -3.3, -2.7, "dB");
        check ("LOW CUT " + std::to_string ((int) fc) + " Hz: gain at 2x the corner" + R, amp (2.0 * fc), -0.35, 0.0, "dB");
        check ("LOW CUT " + std::to_string ((int) fc) + " Hz: gain at half the corner (24 dB/oct)" + R, amp (0.5 * fc), -26.0, -22.0, "dB");
    }
    {
        auto v = voice (fs, 3.0);
        Params p;
        p.heatOn = false;
        p.amountPct = 0.0f;
        auto e = make (fs, p);
        Buf y = v.x;
        float maxIn = 0.0f;
        for (size_t i = 1; i < v.x[0].size(); ++i)
            maxIn = std::max (maxIn, std::abs (v.x[0][i] - v.x[0][i - 1]));
        for (size_t s = 0, k = 0; s < y[0].size(); s += 256, ++k)
        {
            if (k % 40 == 20)
            {
                p.lowCut = (p.lowCut + 1) % 4;
                e->setParams (p);
            }
            float* io[2] = { y[0].data() + s, y[1].data() + s };
            e->process (io, 2, (int) std::min<size_t> (256, y[0].size() - s));
        }
        float maxOut = 0.0f;
        for (size_t i = 1; i < y[0].size(); ++i)
            maxOut = std::max (maxOut, std::abs (y[0][i] - y[0][i - 1]));
        check ("LOW CUT switches while playing: largest sample step re the input's" + R, maxOut / maxIn, 0.0, 1.2, "x");
    }
    // 15. every factory preset: finite, level kept (auto gain), no overload on a nominal vocal
    {
        auto v = voice (fs, 3.0, -14.0);
        double in = 0;
        for (size_t i = (size_t) fs; i < v.x[0].size(); ++i)
            in += (double) v.x[0][i] * v.x[0][i];
        int k = 0;
        for (const auto& pr : factoryPresets())
        {
            Params p = pr.params;
            if (! pr.setsLevel)
                p.levelDb = 0.0f;
            else
                p.levelDb = 0.0f; // the synthetic voice sits at the nominal level already
            if (quick && k++ % 3 != 0)
                continue;
            const auto y = render (fs, p, v.x);
            double out = 0, pk = 0;
            bool fin = true;
            for (size_t i = (size_t) fs; i < y[0].size(); ++i)
            {
                fin = fin && std::isfinite (y[0][i]);
                out += (double) y[0][i] * y[0][i];
                pk = std::max (pk, (double) std::abs (y[0][i]));
            }
            check (std::string ("preset '") + pr.name + "': level change (finite: " + (fin ? "yes" : "NO") + ")" + R,
                   fin ? 10.0 * std::log10 (out / in) - (double) p.outputDb : 99.0, -3.0, 1.0, "dB");
        }
    }
}

// the detector alone: rate and level independence of the probability on the synthetic voice
static void testDetector()
{
    std::string row;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        auto v = voice (fs, 6.0);
        SibilanceDetector d;
        d.prepare (fs);
        std::vector<float> p (v.x[0].size(), 0.0f);
        float last = 0.0f;
        for (size_t i = 0; i < v.x[0].size(); ++i)
        {
            if (d.push (v.x[0][i]))
            {
                last = d.out().p;
                const auto c = (size_t) std::max<int64_t> (0, d.out().center);
                if (c < p.size())
                    p[c] = last + 1.0e-6f; // mark (0 = no frame)
            }
        }
        double pe = 0, ne = 0, pv = 0, nv = 0;
        for (auto [a, b] : v.esses)
            if (a > (size_t) (1.3 * fs))
                for (size_t i = a; i < b; ++i)
                    if (p[i] > 0.0f) { pe += p[i]; ne += 1; }
        for (auto [a, b] : v.vowels)
            if (a > (size_t) (1.3 * fs))
                for (size_t i = a; i < b; ++i)
                    if (p[i] > 0.0f) { pv += p[i]; nv += 1; }
        check ("detector: mean probability on the sibilants @ " + rateName (fs), pe / ne, 0.6, 1.0, "");
        check ("detector: mean probability on the vowels @ " + rateName (fs), pv / nv, 0.0, 0.05, "");
    }
}

static void testAllocAndCpu (bool quick)
{
    // no allocation in process(): parameter changes, mode / oversampling switches, bypass, listen
    {
        const double fs = 48000.0;
        auto v = voice (fs, 3.0);
        Params p;
        auto e = make (fs, p);
        allocCount = 0;
        Buf y = v.x;
        for (size_t s = 0, k = 0; s + 512 <= y[0].size(); s += 512, ++k)
        {
            p.mode = (int) (k / 20) % kNumModes;
            p.osLog2 = 1 + (int) (k / 37) % 4;
            p.drive = (float) (k % 11);
            p.listen = k % 50 > 45;
            p.bypass = k % 70 > 64;
            p.tone = (float) ((int) (k % 9) * 20 - 80);
            e->setParams (p);
            float* io[2] = { y[0].data() + s, y[1].data() + s };
            countAllocs = true;
            e->process (io, 2, 512);
            countAllocs = false;
        }
        check ("no memory allocation in process() (switches, bypass, listen)", (double) allocCount, 0, 0, "allocs");
    }
    if (quick)
        return;
    // CPU, 48 kHz stereo, one core: defaults (16x) and 8x, every mode
    for (int mode = 0; mode < kNumModes; ++mode)
        for (int os : { 4, 3 })
        {
            const double fs = 48000.0;
            auto v = voice (fs, 10.0);
            Params p;
            p.mode = mode;
            p.osLog2 = os;
            auto e = make (fs, p);
            run (*e, Buf (2, std::vector<float> (48000, 0.0f)));
            const auto t0 = std::chrono::steady_clock::now();
            run (*e, v.x, 512);
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            check ("CPU, 48 kHz stereo, mode " + std::to_string (mode) + " " + std::to_string (1 << os) + "x (% of one core)", 100.0 * sec / 10.0,
                   0.0, os == 4 ? 12.0 : 9.0, "%");
        }
}

int main (int argc, char** argv)
{

    const bool quick = argc > 1 && std::strcmp (argv[1], "--quick") == 0;
    const auto t0 = std::chrono::steady_clock::now();
    testDetector();
    for (double fs : quick ? std::vector<double> { 48000.0 } : std::vector<double> { 44100.0, 48000.0, 96000.0 })
        testRate (fs, quick);
    testAllocAndCpu (quick);
    const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    if (fails == 0)
        std::printf ("ALL PASS: %d of %d checks passed (%.1f s)\n", checks, checks, sec);
    else
        std::printf ("FAILED: %d of %d checks (%.1f s)\n", fails, checks, sec);
    return fails;
}
