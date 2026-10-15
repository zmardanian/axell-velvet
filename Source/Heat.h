#pragma once
// Axell Velvet - HEAT: three analog saturation models, run oversampled (framework independent, no JUCE).
//
// Every model takes the driven signal u (u = 1 is 18 dB above the nominal vocal level of -18 dBFS RMS at DRIVE 0)
// and returns it with unity small-signal gain: below the knee the model is a pure (linear) filter, so the auto gain
// around it (pre x post = 1) keeps the level; only the saturation is added.
//
//  TAPE    - record pre-emphasis (50 us / 20 us shelf) -> Jiles-Atherton magnetic hysteresis (Jiles & Atherton,
//            J. Magn. Magn. Mater. 61, 1986), integrated in the field domain with RK4 -> exact inverse de-emphasis.
//            High frequencies reach saturation first (tape's smaller HF headroom): treble and esses are compressed
//            gently, the low end thickens; the loop adds level-dependent phase and odd harmonics.
//  CONSOLE - input transformer (core flux = integral of the voltage; the magnetising current rises steeply with the
//            flux, so low, loud notes saturate first) -> single-ended class-A transistor stage with emitter
//            degeneration (exponential cut-off on one side, the supply rail on the other: 2nd + 3rd harmonics).
//  TUBE    - common-cathode triode stage (Koren's triode equations, B+ 250 V, 100k plate load, cathode bias; the
//            load line is solved once at prepare) with grid conduction above 0 V and the resulting bias shift
//            (the coupling capacitor charges on loud peaks: the operating point moves and the stage gets richer
//            in even harmonics and compresses for ~100 ms afterwards). Plate capacitance: -3 dB at 35 kHz.
#include <axell/dsp/Filters.h>
#include <axell/dsp/Math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace axv
{
using namespace axell;

enum HeatMode : int { kTape = 0, kConsole = 1, kTube = 2, kNumModes = 3 };

namespace heat
{
// first-order section from analog (b0 + b1 s) / (a0 + a1 s), bilinear transform (prewarp-free: the corners are far
// below the oversampled Nyquist)
struct OnePole
{
    double b0 = 1, b1 = 0, a1 = 0;
    std::array<double, kMaxCh> z {};
    void analog (double B0, double B1, double A0, double A1, double fs)
    {
        const double K = 2.0 * fs;
        const double nb0 = B0 + B1 * K, nb1 = B0 - B1 * K, na0 = A0 + A1 * K, na1 = A0 - A1 * K;
        b0 = nb0 / na0;
        b1 = nb1 / na0;
        a1 = na1 / na0;
    }
    double process (double x, int ch) noexcept
    {
        const double y = b0 * x + z[(size_t) ch];
        z[(size_t) ch] = b1 * x - a1 * y;
        return y;
    }
    void reset() noexcept { z.fill (0.0); }
};

// lookup table with linear interpolation over [lo, hi], clamped outside (slopes continue linearly)
struct Table
{
    std::vector<float> v;
    float lo = -1, hi = 1, scale = 1, mid = 0, slopeLo = 0, slopeHi = 0;
    // symmetric range [-r, r], odd n: x = 0 is the middle node, and f(0) is subtracted - silence stays exactly 0
    template <typename F>
    void build (F f, float r, int n)
    {
        n |= 1;
        lo = -r;
        hi = r;
        const float a = lo, b = hi;
        v.resize ((size_t) n);
        const double f0 = f (0.0);
        for (int i = 0; i < n; ++i)
            v[(size_t) i] = (float) (f (a + (b - a) * (double) i / (n - 1)) - f0);
        v[(size_t) n / 2] = 0.0f;
        mid = (float) (n / 2);
        scale = (float) (n - 1) / (b - a);
        slopeLo = (v[1] - v[0]) * scale;
        slopeHi = (v[(size_t) n - 1] - v[(size_t) n - 2]) * scale;
    }
    float operator() (float x) const noexcept
    {
        if (x <= lo)
            return v.front() + (x - lo) * slopeLo;
        if (x >= hi)
            return v.back() + (x - hi) * slopeHi;
        const float p = mid + x * scale; // x = 0 lands exactly on the middle node
        const int i = std::min ((int) p, (int) v.size() - 2);
        const float t = p - (float) i;
        return v[(size_t) i] + t * (v[(size_t) i + 1] - v[(size_t) i]);
    }
};

// L(x) = coth x - 1/x and L'(x) = 1/x^2 - 1/sinh^2 x from one exponential
inline void langevinBoth (double x, double& l, double& dl) noexcept
{
    const double ax = std::abs (x);
    if (ax < 1.0e-2)
    {
        const double x2 = x * x;
        l = x * (1.0 / 3.0 - x2 / 45.0);
        dl = 1.0 / 3.0 - x2 / 15.0;
        return;
    }
    if (ax > 18.0)
    {
        l = (x > 0 ? 1.0 : -1.0) - 1.0 / x;
        dl = 1.0 / (x * x);
        return;
    }
    const double e = std::exp (-2.0 * ax), om = 1.0 - e;
    const double coth = (1.0 + e) / om;            // coth |x|
    l = (x > 0 ? 1.0 : -1.0) * coth - 1.0 / x;
    dl = 1.0 / (x * x) - 4.0 * e / (om * om);      // 1/sinh^2 x = 4 e^{-2|x|} / (1 - e^{-2|x|})^2
}

inline double langevin (double x) noexcept
{
    if (std::abs (x) < 1.0e-3)
        return x / 3.0;
    return 1.0 / std::tanh (x) - 1.0 / x;
}
inline double langevinD (double x) noexcept
{
    if (std::abs (x) < 1.0e-3)
        return 1.0 / 3.0 - x * x / 15.0;
    const double s = std::sinh (x);
    return 1.0 / (x * x) - 1.0 / (s * s);
}
} // namespace heat

//==============================================================================
class TapeModel
{
public:
    // Jiles-Atherton parameters, normalised (Ms = 1): anhysteretic width a, coercivity k, reversibility c (0.995: a
    // well-biased tape - clean at low level), coupling alpha; field scale; record pre-emphasis 50 us / 20 us (+8 dB);
    // the irreversible gate width (DECISIONS V6)
    static constexpr double kA = 1.0, kK = 0.2, kC = 0.995, kAlpha = 0.0, kHd = 2.2, kTauA = 50.0e-6, kTauB = 20.0e-6,
                            kClamp = 1.5, kGate = 0.01;

    void prepare (double fs)
    {
        if (tab.empty())
        {
            tabN = (int) std::lround (2.0 * kTabMax / kTabStep) + 1;
            tab.resize (2 * (size_t) tabN);
            for (int i = 0; i < tabN; ++i)
            {
                double l, d;
                heat::langevinBoth ((-kTabMax + i * kTabStep) / kA, l, d);
                tab[2 * (size_t) i] = l;
                tab[2 * (size_t) i + 1] = d / kA;
            }
            tabScale = 1.0 / kTabStep;
            tabMid = kTabMax / kTabStep;
        }
        substeps = std::max (1, (int) std::ceil (768000.0 / fs - 1.0e-9));
        pre.analog (1.0, kTauA, 1.0, kTauB, fs);
        de.analog (1.0, kTauB, 1.0, kTauA, fs);
        // small-signal slope dM/dH at the origin of the initial curve: c Ms/(3a) / (1 - alpha c Ms/(3a)) is the
        // reversible part; the irreversible part adds (1-c) * (Man - M)/(...) which is 0 at the origin, so normalise
        // on a measured slope instead (a -60 dBFS sine)
        norm = 1.0;
        reset();
        double peakM = 0.0;
        const double f = 1000.0, amp = 1.0e-3;
        for (int i = 0; i < (int) (fs / f) * 4; ++i)
        {
            const double h = amp * kHd * std::sin (2.0 * kPi * f * i / fs);
            const double m = step (h, 0);
            if (i > (int) (fs / f) * 2)
                peakM = std::max (peakM, std::abs (m));
        }
        norm = peakM > 0.0 ? amp / peakM : 1.0;
        reset();
    }

    void reset() noexcept
    {
        pre.reset();
        de.reset();
        M.fill (0.0);
        H.fill (0.0);
    }

    void process (float* x, int n, int ch) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const double h = pre.process ((double) x[i], ch) * kHd;
            x[i] = (float) de.process (step (h, ch) * norm, ch);
        }
    }

private:

    static_assert (kAlpha <= 0.0 && kAlpha >= 0.0, "the Langevin tables assume no inter-domain coupling (alpha = 0)");
    static constexpr double kTabMax = 64.0, kTabStep = 0.005; // |error| of the interpolated L, L' < 2e-6

    // L(h/a) and L'(h/a)/a, tabulated (alpha = 0: they depend on the field only), linear interpolation
    void lookup (double h, double& man, double& dl) const noexcept
    {
        const double x = std::clamp (h, -kTabMax, kTabMax) * tabScale + tabMid;
        const int i = std::min ((int) x, tabN - 2);
        const double t = x - i;
        const double* q = tab.data() + 2 * i; // interleaved L, L': one cache line per lookup
        man = q[0] + t * (q[2] - q[0]);
        dl = q[1] + t * (q[3] - q[1]);
    }

    // Jiles-Atherton dM/dH. With alpha = 0 the general form
    //   [(1-c) dM_gate (Man - M) / ((1-c) delta k - alpha (Man - M)) + c dMan/dH] / (1 - alpha c dMan/dH)
    // reduces exactly to dM_gate (Man - M) / (delta k) + c dMan/dH.
    double dMdH (double h, double m, double delta) const noexcept
    {
        double man, dl;
        lookup (h, man, dl);
        const double dm = man - m;
        // the irreversible term only acts while the magnetisation lags the anhysteretic curve in the direction of the
        // field change; a smooth gate (width kGate) instead of the hard switch avoids a slope kink at every reversal
        // (a kink sprays harmonics far beyond the oversampled band: aliasing)
        const double x = delta * dm / kGate;
        const double gate = 0.5 * (1.0 + x / (1.0 + std::abs (x))); // smooth (C1) and cheap
        const double irr = gate * dm * delta * (1.0 / kK);
        return std::max (0.0, irr + kC * dl);
    }

    // one oversampled sample: the field moves from h0 to h in `substeps` Heun steps (the solver always runs at
    // >= 768 kHz, whatever the oversampling: the smooth gate makes the equation stiffer than the hard switch)
    double step (double h, int ch) noexcept
    {
        auto& m = M[(size_t) ch];
        double& h0 = H[(size_t) ch];
        const double dh = (h - h0) / substeps;
        if (std::abs (dh) > 0.0)
        {
            const double d = dh > 0.0 ? 1.0 : -1.0;
            double ha = h0;
            for (int s = 0; s < substeps; ++s)
            {
                const double hb = ha + dh;
#if AXV_TAPE_RK4
                const double k1 = dMdH (ha, m, d);
                const double k2 = dMdH (ha + 0.5 * dh, m + 0.5 * dh * k1, d);
                const double k3 = dMdH (ha + 0.5 * dh, m + 0.5 * dh * k2, d);
                const double k4 = dMdH (hb, m + dh * k3, d);
                m += dh / 6.0 * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
#else
                const double k1 = dMdH (ha, m, d);
                const double k2 = dMdH (hb, m + dh * k1, d);
                m += 0.5 * dh * (k1 + k2);
#endif
                ha = hb;
            }
            m = std::clamp (m, -kClamp, kClamp);
        }
        h0 = h;
        return m;
    }

    heat::OnePole pre, de;
    std::array<double, kMaxCh> M {}, H {};
    double norm = 1.0, tabScale = 1.0, tabMid = 0.0;
    int tabN = 0, substeps = 1;
    std::vector<double> tab;
};

//==============================================================================
class ConsoleModel
{
public:
    static constexpr double kHpfHz = 6.0;     // transformer low-frequency corner (small signal)
    static constexpr double kSatHz = 45.0;    // a u = 1 sine at this frequency reaches the saturation flux
    static constexpr double kSatK = 24.0;     // magnetising-current rise at the saturation flux
    static constexpr double kR = 3.0;         // emitter degeneration I0 Re / Vt
    static constexpr double kG = 3.6;         // drive into the transistor at u = 1

    void prepare (double fs)
    {
        T = 1.0 / fs;
        wc = 2.0 * kPi * kHpfHz;
        phiS = 1.0 / (2.0 * kPi * kSatHz);
        // class-A stage: R (i - 1) + ln i = g v  (normalised collector current i, quiescent 1), the rail at i = 2.15
        auto current = [] (double gv)
        {
            double i = 1.0;
            for (int it = 0; it < 60; ++it)
            {
                const double f = kR * (i - 1.0) + std::log (i) - gv, df = kR + 1.0 / i;
                double ni = i - f / df;
                if (ni <= 0.0)
                    ni = 0.5 * i;
                if (std::abs (ni - i) < 1.0e-13)
                {
                    i = ni;
                    break;
                }
                i = ni;
            }
            const double rail = 2.15, knee = 0.25; // soft saturation into the rail
            const double over = i - (rail - knee);
            if (over > 0.0)
                i = rail - knee + knee * std::tanh (over / knee);
            return i;
        };
        const double slope = kG / (kR + 1.0);
        stage.build ([&] (double v) { return (current (kG * v) - 1.0) / slope; }, 4.0f, 8193);
        reset();
    }

    void reset() noexcept
    {
        phi.fill (0.0);
        dc.fill (0.0);
    }

    void process (float* x, int n, int ch) noexcept
    {
        double& p = phi[(size_t) ch];
        double& d = dc[(size_t) ch];
        const double dcK = 1.0 - std::exp (-2.0 * kPi * 3.0 * T);
        for (int i = 0; i < n; ++i)
        {
            const double v = x[i];
            // transformer: v_core = v - w_c phi (1 + K (phi/phiS)^2), phi' = v_core (trapezoid, one fixed-point pass)
            auto core = [&] (double ph) { const double r = ph / phiS; return v - wc * ph * (1.0 + kSatK * r * r); };
            const double vc0 = core (p);
            const double pPred = p + T * vc0;
            const double vc = 0.5 * (vc0 + core (pPred));
            p += T * vc;
            // class-A stage, then the output coupling (DC blocker, 3 Hz)
            const double y = stage ((float) vc);
            d += dcK * (y - d);
            x[i] = (float) (y - d);
        }
    }

private:
    double T = 1.0 / 192000.0, wc = 1.0, phiS = 1.0;
    heat::Table stage;
    std::array<double, kMaxCh> phi {}, dc {};
};

//==============================================================================
class TubeModel
{
public:
    // Koren triode (12AX7-like): mu 100, Ex 1.4, Kg1 1060, Kp 600, Kvb 300; B+ 250 V, plate load 100k, cathode 1.5k
    static constexpr double kMu = 100.0, kEx = 1.4, kKg1 = 1060.0, kKp = 600.0, kKvb = 300.0;
    static constexpr double kBplus = 250.0, kRa = 100.0e3, kRk = 1500.0;
    static constexpr double kVin = 2.4;       // grid volts at u = 1
    static constexpr double kGridV = 0.25;    // grid conduction knee (V)
    static constexpr double kChargeMs = 2.0, kReleaseMs = 120.0, kShift = 0.35;

    static double plateCurrent (double vgk, double vp) noexcept
    {
        const double e = kKp * (1.0 / kMu + vgk / std::sqrt (kKvb + vp * vp));
        const double soft = e > 30.0 ? e : std::log1p (std::exp (e));
        const double e1 = vp / kKp * soft;
        return e1 > 0.0 ? 2.0 * std::pow (e1, kEx) / kKg1 : 0.0;
    }
    // plate voltage on the load line for a grid-cathode voltage (bisection: monotone)
    static double plateVoltage (double vgk) noexcept
    {
        double lo = 0.0, hi = kBplus;
        for (int it = 0; it < 80; ++it)
        {
            const double vp = 0.5 * (lo + hi);
            if (vp + kRa * plateCurrent (vgk, vp) > kBplus)
                hi = vp;
            else
                lo = vp;
        }
        return 0.5 * (lo + hi);
    }

    void prepare (double fs)
    {
        T = 1.0 / fs;
        // cathode bias operating point: vk = Rk * Ip(-vk, vp(-vk))
        double vk = 1.0;
        for (int it = 0; it < 100; ++it)
            vk = 0.5 * vk + 0.5 * kRk * plateCurrent (-vk, plateVoltage (-vk));
        bias = -vk;
        vp0 = plateVoltage (bias);
        const double h = 1.0e-4;
        gain0 = (plateVoltage (bias - h) - plateVoltage (bias + h)) / (2.0 * h); // inverting stage: positive number
        // table over the applied grid swing (relative to the bias), grid conduction folded in
        curve.build ([&] (double u)
                     {
                         double vg = bias + u * kVin;
                         if (vg > 0.0) // grid current: the source cannot drive the grid much above the cathode
                             vg = kGridV * std::tanh (vg / kGridV);
                         return (vp0 - plateVoltage (vg)) / (gain0 * kVin);
                     },
                     (float) (8.0 / kVin), 8193);
        lp.analog (1.0, 0.0, 1.0, 1.0 / (2.0 * kPi * 35000.0), fs);
        chargeK = 1.0 - std::exp (-T / (kChargeMs * 0.001));
        relK = 1.0 - std::exp (-T / (kReleaseMs * 0.001));
        dcK = 1.0 - std::exp (-2.0 * kPi * 3.0 * T);
        reset();
    }

    void reset() noexcept
    {
        shift.fill (0.0);
        dc.fill (0.0);
        lp.reset();
    }

    double biasVolts() const noexcept { return bias; }
    double smallSignalGain() const noexcept { return gain0; }

    void process (float* x, int n, int ch) noexcept
    {
        double& s = shift[(size_t) ch];
        double& d = dc[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            const double u = x[i] - s / kVin;
            // grid conduction charges the coupling capacitor (operating point moves negative), it leaks back slowly
            const double cond = std::max (0.0, bias + u * kVin);
            s += chargeK * kShift * cond - relK * s;
            const double y = lp.process (curve ((float) u), ch);
            d += dcK * (y - d);
            x[i] = (float) (y - d);
        }
    }

private:
    double T = 1.0 / 192000.0, bias = -1.5, vp0 = 150.0, gain0 = 60.0, chargeK = 0.0, relK = 0.0, dcK = 0.0;
    heat::Table curve;
    heat::OnePole lp;
    std::array<double, kMaxCh> shift {}, dc {};
};
} // namespace axv
