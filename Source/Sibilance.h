#pragma once
// Axell Velvet - sibilance detector and de-esser (framework independent, no JUCE).
//
// DETECTOR (per 2 ms hop, on the mono sum): a 12 ms Hann STFT and a YIN aperiodicity measure (de Cheveigne & Kawahara,
// JASA 2002) on the 1.5 kHz low-passed voice give ten features: aperiodicity, HF spectral flatness, level against the
// voice's own loud level, six band levels and the 1.5-14 kHz total against the voice body (100-1500 Hz). The band
// features are adapted to the singer (minus their running mean over voiced, loud frames, 3 s) and smoothed over +-2
// hops; with five of them averaged over the past 48 ms (how long the noise has lasted: an "s" lasts, a "t" burst does
// not) they feed a small neural network (19 -> 32 tanh -> 1; DetectorModel.h, trained on these exact features:
// research/train_cpp.py). Output: the probability that the frame is a sibilant (s, z, sh, zh, ch, j). Level
// independent by construction.
//
// DE-ESSER: the voice-body level of voiced, loud frames is tracked (300 ms). The sibilant's 2-17 kHz energy may reach
// body + AMOUNT dB; above that, water-filling finds the per-band cap that brings the total down to the target by
// flattening only the sibilance's own peaks (a hiss centred at 8 kHz is cut at 8 kHz, a "sh" at 3 kHz at 3 kHz).
// Gains are range-limited, weighted by p^2 (no cut where the detector is unsure), look ahead 2 hops (attack before
// the onset) and release in 12 ms. They are applied with ten complementary linear-phase bands (Kaiser FIR,
// sum = a pure delay): one composite FIR per hop, crossfaded sample by sample. With no reduction the output is the
// input delayed by latency(), bit for bit.
// LIVE (DetectorConfig::live()): a causal detector (its own network) and no look-ahead; the same water-filling gains
// become a minimum-phase filter with STUDIO's magnitude (real cepstrum), so the plugin adds no latency. Bit for bit the
// input when there is nothing to cut.
//
// Real-time safety: prepare() allocates; push() / process() never allocate, lock or throw.
#include <axell/dsp/Fft.h>
#include <axell/dsp/Filters.h>
#include <axell/dsp/Math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#ifdef AXV_MODEL_HEADER
 #include AXV_MODEL_HEADER // research builds: held-out models
#else
 #include "DetectorModel.h"
#endif
#ifdef AXV_LIVE_MODEL_HEADER
 #include AXV_LIVE_MODEL_HEADER
#else
 #include "DetectorModelLive.h"
#endif

namespace axv
{
using namespace axell;

constexpr int kNumFeat = 10;   // ap, flatness, level re loud, 6 bands re body, 1.5-14 kHz re body
constexpr int kNumBase = 14;               // per-frame model inputs
constexpr int kNumCtx = 5;                 // inputs also averaged over the past kCtxFrames
constexpr int kCtxIdx[kNumCtx] = { 0, 1, 9, 10, 11 };
constexpr int kCtxFrames = 24;
constexpr int kNumDesign = kNumBase + kNumCtx;
static_assert (kNumDesign == model::kInputs, "DetectorModel.h does not match the detector");
static_assert (kNumDesign == model_live::kInputs, "DetectorModelLive.h does not match the detector");

// a network's weights (the studio and the LIVE detector have one each)
struct ModelRef
{
    int hidden;
    const float *mu, *sd, *w1, *b1, *w2;
    float b2;
};
inline constexpr ModelRef kStudioModel { model::kHidden, model::kMu, model::kSd, model::kW1, model::kB1, model::kW2, model::kB2 };
inline constexpr ModelRef kLiveModel { model_live::kHidden, model_live::kMu, model_live::kSd, model_live::kW1, model_live::kB1, model_live::kW2, model_live::kB2 };
constexpr int kNumBands = 10;  // de-esser bands
constexpr double kEdges[kNumBands + 1] = { 2000, 2800, 3600, 4500, 5600, 7000, 8500, 10000, 12000, 14500, 17000 };
constexpr double kFeatBands[6][2] = { { 1500, 2500 }, { 2500, 3500 }, { 3500, 5000 }, { 5000, 7000 }, { 7000, 10000 }, { 10000, 14000 } };
constexpr int kFocusSplit = 4; // FOCUS S: bands 4.. (5.6 kHz up); FOCUS SH: bands ..4 (below 5.6 kHz)

struct DetectorConfig
{
    double winMs = 12.0, hopMs = 2.0;
    int smooth = 2;                    // feature smoothing +-hops (causal: the past 2 * smooth + 1 frames)
    double yinFmin = 70.0, yinFmax = 500.0, yinThresh = 0.15;
    // LIVE: every frame is reported as soon as its window is complete, features are smoothed over the past only, and
    // the LIVE network (trained on exactly these features) decides
    bool causal = false;
    static DetectorConfig live() noexcept
    {
        DetectorConfig c;
        c.causal = true;
        c.smooth = 1;
        return c;
    }
};

struct DetFrame
{
    int64_t center = 0;                // input sample index of the frame centre
    float feat[kNumFeat] {};           // smoothed raw features
    float adapted[kNumFeat] {};        // smoothed speaker-adapted features
    float x[kNumDesign] {};            // model inputs
    float p = 0.0f;                    // sibilance probability
    float E[kNumBands] {};             // de-esser band energies (power, unsmoothed)
    float bodyDb = -140.0f, levelDb = -140.0f, f0 = 0.0f, apRaw = 1.0f;
};

namespace detail
{
inline double besselI0 (double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < 1.0e-12 * s)
            break;
    }
    return s;
}
} // namespace detail

//==============================================================================
class SibilanceDetector
{
public:
    void prepare (double sampleRate, const DetectorConfig& c = {})
    {
        fs = sampleRate;
        cfg = c;
        net = cfg.causal ? &kLiveModel : &kStudioModel;
        hop = std::max (1, (int) std::lround (cfg.hopMs * 0.001 * fs));
        win = std::max (16, (int) std::lround (cfg.winMs * 0.001 * fs));
        nfft = 16;
        while (nfft < win)
            nfft <<= 1;
        fft.prepare (nfft);
        window.resize ((size_t) win);
        double wsum = 0.0;
        for (int i = 0; i < win; ++i)
        {
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (i + 0.5) / win));
            wsum += window[(size_t) i];
        }
        pScale = (float) (1.0 / (wsum * wsum));
        frameIn.assign ((size_t) nfft, 0.0f);
        spec.assign ((size_t) nfft + 2, 0.0f);
        power.assign ((size_t) nfft / 2 + 1, 0.0f);
        ringSize = 1;
        while (ringSize < win + 2)
            ringSize <<= 1;
        ring.assign ((size_t) ringSize, 0.0f);
        auto bin = [this] (double hz) { return std::clamp ((int) std::ceil (hz * nfft / fs), 0, nfft / 2 + 1); };
        auto range = [&] (double lo, double hi) { return std::array<int, 2> { bin (lo), std::max (bin (lo), bin (std::min (hi, 0.5 * fs))) }; };
        bodyR = range (100.0, 1500.0);
        flatR = range (2500.0, 14000.0);
        hfR = range (1500.0, 14000.0);
        for (int b = 0; b < 6; ++b)
            featR[(size_t) b] = range (kFeatBands[b][0], kFeatBands[b][1]);
        for (int b = 0; b < kNumBands; ++b)
            deR[(size_t) b] = range (kEdges[b], kEdges[b + 1]);

        // YIN: 8th-order Butterworth at 1.5 kHz, decimated to ~8 kHz
        dec = std::max (1, (int) std::lround (fs / 8000.0));
        fd = fs / dec;
        static constexpr double q[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
        for (int s = 0; s < 4; ++s)
            lp[(size_t) s].setLowPass (fs, 1500.0, q[s]);
        yW = std::max (8, (int) std::lround (cfg.winMs * 0.001 * fd));
        yTmax = (int) std::ceil (fd / cfg.yinFmin) + 1;
        yTmin = std::max (2, (int) std::floor (fd / cfg.yinFmax));
        dRingSize = 1;
        while (dRingSize < yW + yTmax + 8)
            dRingSize <<= 1;
        dRing.assign ((size_t) dRingSize, 0.0f);
        yLin.assign ((size_t) (yW + yTmax), 0.0f);
        dfun.assign ((size_t) yTmax + 1, 0.0);
        dnorm.assign ((size_t) yTmax + 1, 1.0);

        adaptK = (float) (1.0 / 1500.0 * (cfg.hopMs / 2.0)); // tau 3 s
        hist.assign ((size_t) (2 * cfg.smooth + 1), Raw {});
        reset();
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (dRing.begin(), dRing.end(), 0.0f);
        for (auto& f : lp)
            f.resetState();
        n = 0;
        nd = 0;
        hopCount = 0;
        loudRef = -140.0f;
        loudInit = false;
        baseInit = false;
        base.fill (0.0f);
        histCount = 0;
        histPos = 0;
        frames = 0;
        for (auto& c : ctx)
            c.fill (0.0f);
        ctxPos = ctxCount = 0;
    }

    int hopSize() const noexcept { return hop; }
    int windowSize() const noexcept { return win; }
    int smoothing() const noexcept { return cfg.smooth; }
    double sampleRate() const noexcept { return fs; }
    // samples from the newest input to the centre of the frame push() returns
    int frameDelay() const noexcept { return win / 2 + (cfg.causal ? 0 : cfg.smooth * hop); }
    bool causal() const noexcept { return cfg.causal; }

    // feed one mono sample; true when out() holds a new frame (centre = newest - frameDelay())
    bool push (float x) noexcept
    {
        ring[(size_t) (n & (ringSize - 1))] = x;
        double y = x;
        for (auto& f : lp)
            y = f.process (y, 0);
        if (n % dec == 0)
        {
            dRing[(size_t) (nd & (dRingSize - 1))] = (float) y;
            ++nd;
        }
        ++n;
        if (++hopCount < hop)
            return false;
        hopCount = 0;
        analyse();
        return frames > (cfg.causal ? 0 : cfg.smooth);
    }

    const DetFrame& out() const noexcept { return result; }

    // a network on the design vector
    static float probability (const float* x, const ModelRef& m = kStudioModel) noexcept
    {
        float z[kNumDesign];
        for (int i = 0; i < kNumDesign; ++i)
            z[i] = (x[i] - m.mu[i]) / m.sd[i];
        float o = m.b2;
        for (int j = 0; j < m.hidden; ++j)
        {
            float a = m.b1[j];
            for (int i = 0; i < kNumDesign; ++i)
                a += z[i] * m.w1[i * m.hidden + j];
            o += m.w2[j] * std::tanh (a);
        }
        o = std::clamp (o, -30.0f, 30.0f);
        return 1.0f / (1.0f + std::exp (-o));
    }

private:
    struct Raw
    {
        int64_t center = 0;
        float F[kNumFeat] {}, A[kNumFeat] {}, E[kNumBands] {};
        float bodyDb = -140.0f, levelDb = -140.0f, f0 = 0.0f;
    };

    float bandSum (const std::array<int, 2>& r) const noexcept
    {
        double s = 0.0;
        for (int k = r[0]; k < r[1]; ++k)
            s += power[(size_t) k];
        return (float) s + 1.0e-24f; // a floor far below any recording (level independence)
    }

    void yin (int64_t center, float& ap, float& f0) noexcept
    {
        ap = 1.0f;
        f0 = 0.0f;
        // decimated index of the frame centre; the window [c - W/2, c + W/2) against lags into the past
        const int64_t c = center / dec;
        const int64_t j0 = c - yW / 2, j1 = j0 + yW;
        if (j0 - yTmax < 0 || j1 > nd)
            return;
        // the window and its lags as one straight run (values unchanged): lin[i] = x[j0 - yTmax + i]
        const int mask = dRingSize - 1;
        const int nLin = yW + yTmax;
        for (int i = 0; i < nLin; ++i)
            yLin[(size_t) i] = dRing[(size_t) ((j0 - yTmax + i) & mask)];
        const float* xw = yLin.data() + yTmax;            // xw[j - j0] = x[j]
        double e0 = 0.0;
        for (int j = 0; j < yW; ++j)
            e0 += (double) xw[j] * xw[j];
        if (e0 < 1.0e-24) // digital silence only
            return;
        // difference function d(t) = sum_j (x[j] - x[j - t])^2: the lags side by side, each one summed over j in the
        // same order as before (bit-identical), so the inner loop over t vectorises
        // eight lags at a time held in registers (register blocking); every lag still sums over j in the original order
        dfun[0] = 0.0;
        int t0 = 1;
        for (; t0 + 8 <= yTmax + 1; t0 += 8)
        {
            double acc[8] = {};
            for (int j = 0; j < yW; ++j)
            {
                const double xj = (double) xw[j];
                const float* xl = xw + j - t0;              // xl[-u] = x[j - (t0 + u)]
                for (int u = 0; u < 8; ++u)
                {
                    const double e = xj - (double) xl[-u];
                    acc[u] += e * e;
                }
            }
            for (int u = 0; u < 8; ++u)
                dfun[(size_t) (t0 + u)] = acc[u];
        }
        for (int t = t0; t <= yTmax; ++t)
        {
            double d = 0.0;
            for (int j = 0; j < yW; ++j)
            {
                const double e = (double) xw[j] - (double) xw[j - t];
                d += e * e;
            }
            dfun[(size_t) t] = d;
        }
        double cum = 0.0;
        dnorm[0] = 1.0;
        for (int t = 1; t <= yTmax; ++t)
        {
            const double d = dfun[(size_t) t];
            cum += d;
            dnorm[(size_t) t] = cum > 1.0e-30 ? d * t / cum : 1.0;
        }
        int k = -1;
        for (int t = yTmin; t < yTmax; ++t)
            if (dnorm[(size_t) t] < cfg.yinThresh)
            {
                k = t;
                while (k + 1 < yTmax && dnorm[(size_t) k + 1] < dnorm[(size_t) k])
                    ++k;
                break;
            }
        if (k < 0)
        {
            k = yTmin;
            for (int t = yTmin; t < yTmax; ++t)
                if (dnorm[(size_t) t] < dnorm[(size_t) k])
                    k = t;
        }
        double tau = k;
        if (k >= 1 && k < yTmax)
        {
            const double y0 = dnorm[(size_t) k - 1], y1 = dnorm[(size_t) k], y2 = dnorm[(size_t) k + 1];
            const double den = y0 - 2.0 * y1 + y2;
            if (den > 0.0)
                tau += std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5);
        }
        ap = (float) std::min (1.0, dnorm[(size_t) std::clamp ((int) std::lround (tau), 0, yTmax)]);
        f0 = (float) (fd / tau);
    }

    void analyse() noexcept
    {
        const int64_t newest = n - 1;
        Raw r;
        r.center = newest - win / 2;
        // STFT frame
        const int mask = ringSize - 1;
        for (int i = 0; i < win; ++i)
            frameIn[(size_t) i] = ring[(size_t) ((newest - win + 1 + i) & mask)] * window[(size_t) i];
        std::fill (frameIn.begin() + win, frameIn.end(), 0.0f);
        fft.forward (frameIn.data(), spec.data());
        double total = 0.0;
        for (int k = 0; k <= nfft / 2; ++k)
        {
            const float p = (spec[(size_t) (2 * k)] * spec[(size_t) (2 * k)] + spec[(size_t) (2 * k + 1)] * spec[(size_t) (2 * k + 1)]) * pScale;
            power[(size_t) k] = p;
            total += p;
        }
        const float body = bandSum (bodyR);
        // spectral flatness: geometric over arithmetic mean, with a floor relative to the frame (level independent)
        const int nf = std::max (1, flatR[1] - flatR[0]);
        double asum = 0.0;
        for (int k = flatR[0]; k < flatR[1]; ++k)
            asum += (double) power[(size_t) k];
        const double floorP = 1.0e-10 * asum / nf + 1.0e-30;
        double lsum = 0.0;
        asum = 0.0;
        for (int k = flatR[0]; k < flatR[1]; ++k)
        {
            lsum += std::log ((double) power[(size_t) k] + floorP);
            asum += (double) power[(size_t) k] + floorP;
        }
        const float flat = (float) (std::exp (lsum / nf) / (asum / nf));
        const float lvl = (float) (10.0 * std::log10 (total + 1.0e-24));
        // loud reference: running 95th percentile of the frame level (quantile tracker, 0.5 dB steps)
        if (lvl > -100.0f)
        {
            if (! loudInit)
            {
                loudRef = lvl;
                loudInit = true;
            }
            else
                loudRef += 0.5f * (lvl > loudRef ? 0.95f : -0.05f);
        }
        float ap, f0;
        yin (r.center, ap, f0);
        r.F[0] = ap;
        r.F[1] = flat;
        r.F[2] = lvl - (loudInit ? loudRef : lvl);
        for (int b = 0; b < 6; ++b)
            r.F[3 + b] = 10.0f * std::log10 (bandSum (featR[(size_t) b]) / body);
        r.F[9] = 10.0f * std::log10 (bandSum (hfR) / body);
        for (int b = 0; b < kNumBands; ++b)
            r.E[b] = bandSum (deR[(size_t) b]);
        r.bodyDb = 10.0f * std::log10 (body);
        r.levelDb = lvl;
        r.f0 = f0;
        // speaker adaptation (causal): band features minus their running mean over voiced, loud frames
        if (r.F[0] < 0.2f && r.F[2] > -25.0f)
        {
            if (! baseInit)
            {
                for (int i = 3; i < kNumFeat; ++i)
                    base[(size_t) i] = r.F[i];
                baseInit = true;
            }
            else
                for (int i = 3; i < kNumFeat; ++i)
                    base[(size_t) i] += adaptK * (r.F[i] - base[(size_t) i]);
        }
        for (int i = 0; i < kNumFeat; ++i)
            r.A[i] = (i >= 3 && baseInit) ? r.F[i] - base[(size_t) i] : r.F[i];
        apLast = ap;

        // history for the centred smoothing; output the frame `smooth` hops back
        const int H = (int) hist.size();
        hist[(size_t) histPos] = r;
        histPos = (histPos + 1) % H;
        histCount = std::min (histCount + 1, H);
        ++frames;
        if (! cfg.causal && frames <= cfg.smooth)
            return;
        // centred: the frame `smooth` hops back, smoothed over +-smooth; causal: the newest, smoothed over the past
        const Raw& mid = hist[(size_t) ((histPos - 1 - (cfg.causal ? 0 : cfg.smooth) + 2 * H) % H)];
        DetFrame& o = result;
        o.center = mid.center;
        for (int i = 0; i < kNumFeat; ++i)
        {
            float sf = 0.0f, sa = 0.0f;
            for (int k = 0; k < histCount; ++k)
            {
                sf += hist[(size_t) k].F[i];
                sa += hist[(size_t) k].A[i];
            }
            o.feat[i] = sf / (float) histCount;
            o.adapted[i] = sa / (float) histCount;
        }
        for (int b = 0; b < kNumBands; ++b)
            o.E[b] = mid.E[b];
        o.bodyDb = mid.bodyDb;
        o.levelDb = mid.levelDb;
        o.f0 = mid.f0;
        o.apRaw = mid.F[0];
        // design: G[:3] (smoothed raw), H[3:] (smoothed adapted), peak band, sh rise, interactions
        float* x = o.x;
        x[0] = o.feat[0];
        x[1] = o.feat[1];
        x[2] = o.feat[2];
        for (int i = 3; i < kNumFeat; ++i)
            x[i] = o.adapted[i];
        float peak = o.adapted[3];
        for (int i = 4; i < 9; ++i)
            peak = std::max (peak, o.adapted[i]);
        x[10] = peak;
        x[11] = std::max (o.adapted[4], o.adapted[5]) - o.adapted[3];
        x[12] = o.feat[0] * peak;
        x[13] = o.feat[1] * peak;
        // context: past kCtxFrames (this one included) of five inputs
        auto& slotC = ctx[(size_t) ctxPos];
        for (int i = 0; i < kNumCtx; ++i)
            slotC[(size_t) i] = x[kCtxIdx[i]];
        ctxPos = (ctxPos + 1) % kCtxFrames;
        ctxCount = std::min (ctxCount + 1, kCtxFrames);
        for (int i = 0; i < kNumCtx; ++i)
        {
            float sum = 0.0f; // summed afresh every frame: no drift over hours
            for (int k = 0; k < ctxCount; ++k)
                sum += ctx[(size_t) k][(size_t) i];
            x[kNumBase + i] = sum / (float) ctxCount;
        }
        o.p = probability (x, *net);
    }

    double fs = 48000.0, fd = 8000.0;
    DetectorConfig cfg;
    const ModelRef* net = &kStudioModel;
    int hop = 96, win = 576, nfft = 1024, ringSize = 1024, dec = 6, yW = 96, yTmax = 115, yTmin = 16, dRingSize = 256;
    RealFft fft;
    std::vector<float> window, frameIn, spec, power, ring, dRing, yLin;
    std::vector<double> dfun, dnorm;
    float pScale = 1.0f;
    std::array<int, 2> bodyR {}, flatR {}, hfR {};
    std::array<std::array<int, 2>, 6> featR {};
    std::array<std::array<int, 2>, kNumBands> deR {};
    std::array<Biquad, 4> lp;
    int64_t n = 0, nd = 0;
    int hopCount = 0;
    float loudRef = -140.0f, adaptK = 1.0f / 1500.0f, apLast = 1.0f;
    bool loudInit = false, baseInit = false;
    std::array<float, kNumFeat> base {};
    std::vector<Raw> hist;
    int histCount = 0, histPos = 0;
    int64_t frames = 0;
    std::array<std::array<float, kNumCtx>, kCtxFrames> ctx {};
    int ctxPos = 0, ctxCount = 0;
    DetFrame result;
};

//==============================================================================
struct DeEssParams
{
    float amountPct = 50.0f;   // 0 = off; the allowed sibilance level re the voice body: amountToDb()
    float rangeDb = 12.0f;     // maximum cut per band
    int focus = 0;             // 0 auto (all bands), 1 S (5.6 kHz up), 2 SH (below 5.6 kHz)
    bool listen = false;       // output what is removed
};

struct DeEssDisplay
{
    float p = 0.0f;            // sibilance probability of the frame being applied
    float hfReBodyDb = -60.0f; // 2-17 kHz energy against the voice-body reference, before
    float targetDb = 0.0f;     // allowed level (AMOUNT) against the same reference
    float grDb = 0.0f;         // energy-weighted HF reduction (positive)
    float bandGrDb[kNumBands] {};
};

class DeEsser
{
public:
    static constexpr int kLook = 2;           // look-ahead in hops
    static constexpr double kTransHz = 500.0; // FIR band transition width
    static constexpr double kStopDb = 70.0;

    // the allowed 2-17 kHz level against the voice body's (peak) level: 0 % +6 dB (barely), 50 % -12 dB, 100 % -30 dB
    static float amountToDb (float pct) noexcept { return 6.0f - 0.36f * std::clamp (pct, 0.0f, 100.0f); }

    void prepare (double sampleRate, int numChannels, const DetectorConfig& c = {})
    {
        fs = sampleRate;
        numCh = std::clamp (numChannels, 1, kMaxCh);
        det.prepare (fs, c);
        hop = det.hopSize();
        live = c.causal;
        // LIVE: no delay at all - the gains of the newest frame act on the newest sample, through a minimum-phase filter
        delay = live ? 0 : det.frameDelay() + kLook * hop;
        designBank();
        designMinPhase();
        ringSize = 1;
        while (ringSize < (live ? taps : delay + half) + hop + 4)
            ringSize <<= 1;
        for (auto& r : ring)
            r.assign ((size_t) (2 * ringSize), 0.0f);
        hOld.assign ((size_t) taps, 0.0f);
        hNew.assign ((size_t) taps, 0.0f);
        relK = (float) (1.0 - std::exp (-(double) hop / (0.012 * fs)));
        refK = (float) (1.0 - std::exp (-(double) hop / (0.4 * fs)));
        reset();
    }

    void reset() noexcept
    {
        det.reset();
        for (auto& r : ring)
            std::fill (r.begin(), r.end(), 0.0f);
        wpos = 0;
        std::fill (hOld.begin(), hOld.end(), 0.0f);
        std::fill (hNew.begin(), hNew.end(), 0.0f);
        oldIdentity = newIdentity = true;
        ramp = hop;
        for (auto& q : look)
            q.fill (0.0f);
        lookPos = 0;
        smoothDb.fill (0.0f);
        bodyRef = -140.0f;
        bodyInit = false;
        bodyQ = -140.0f;
        bodyQInit = false;
        periodicRun = 0;
        pApplied = pPrev = 0.0f;
        for (auto& q : pLook)
            q = 0.0f;
        disp = {};
    }

    int latency() const noexcept { return delay; }
    bool isLive() const noexcept { return live; }
    int firTaps() const noexcept { return taps; }
    const SibilanceDetector& detector() const noexcept { return det; }
    void setParams (const DeEssParams& p) noexcept { params = p; }
    const DeEssDisplay& display() const noexcept { return disp; }
    bool frameReady() const noexcept { return newFrame; }
    // sibilance probability at the output sample just produced (linear between frames)
    float sibilance() const noexcept { return pPrev + (pApplied - pPrev) * std::min (1.0f, (float) ramp / (float) hop); }

    // in-place, n samples, numCh channels (<= prepared). The sum drives the detector; all channels get the same gains.
    // newFrame is set if a frame was analysed during this call (for display).
    void process (float* const* io, int nch, int n, float* sibOut = nullptr) noexcept
    {
        nch = std::min (nch, numCh);
        newFrame = false;
        const float inv = 1.0f / (float) nch;
        for (int i = 0; i < n; ++i)
        {
            float m = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                const float v = io[c][i];
                m += v;
                ring[(size_t) c][(size_t) wpos] = v;
                ring[(size_t) c][(size_t) (wpos + ringSize)] = v;
            }
            if (det.push (m * inv))
            {
                update (det.out());
                newFrame = true;
            }
            if (live)
            {
                // LIVE: the newest sample through the minimum-phase filter (the newest taps samples), crossfaded from the
                // previous hop's filter like STUDIO's; with nothing to cut the sample passes untouched, bit for bit
                const int start = (wpos - taps + 1 + ringSize) & (ringSize - 1);
                const float w = std::min (1.0f, (float) (ramp + 1) / (float) hop);
                for (int c = 0; c < nch; ++c)
                {
                    const float* xw = ring[(size_t) c].data() + start;
                    const float x0 = ring[(size_t) c][(size_t) wpos];
                    float y = x0;
                    if (! (oldIdentity && newIdentity))
                    {
                        const float yo = oldIdentity ? x0 : dot (hOld.data(), xw);
                        const float yn = newIdentity ? x0 : dot (hNew.data(), xw);
                        y = w < 1.0f ? yo + (yn - yo) * w : yn;
                    }
                    io[c][i] = params.listen ? x0 - y : y;
                }
                if (sibOut != nullptr)
                    sibOut[i] = pPrev + (pApplied - pPrev) * w;
                if (ramp < hop)
                    ++ramp;
                wpos = (wpos + 1) & (ringSize - 1);
                continue;
            }
            // output time t = newest - delay; FIR window [t - half, t + half]
            const int t = (wpos - delay + ringSize) & (ringSize - 1);
            const int start = (t - half + ringSize) & (ringSize - 1);
            const float w = std::min (1.0f, (float) (ramp + 1) / (float) hop);
            for (int c = 0; c < nch; ++c)
            {
                const float* xw = ring[(size_t) c].data() + start;
                const float xd = ring[(size_t) c][(size_t) t];
                float diff = 0.0f;
                if (! oldIdentity && w < 1.0f)
                    diff += (1.0f - w) * dot (hOld.data(), xw);
                if (! newIdentity)
                    diff += w * dot (hNew.data(), xw);
                io[c][i] = params.listen ? -diff : xd + diff;
            }
            if (sibOut != nullptr)
                sibOut[i] = pPrev + (pApplied - pPrev) * w;
            if (ramp < hop)
                ++ramp;
            wpos = (wpos + 1) & (ringSize - 1);
        }
    }

private:
    float dot (const float* h, const float* x) const noexcept
    {
        float a[8] {};
        int k = 0;
        for (; k + 8 <= taps; k += 8)
            for (int j = 0; j < 8; ++j)
                a[j] += h[k + j] * x[k + j];
        float s = 0.0f;
        for (; k < taps; ++k)
            s += h[k] * x[k];
        return s + ((a[0] + a[4]) + (a[1] + a[5])) + ((a[2] + a[6]) + (a[3] + a[7]));
    }

    void designBank()
    {
        // Kaiser lowpasses at every edge (odd length, same centre): band b = LP(e[b+1]) - LP(e[b])
        const double A = kStopDb, beta = 0.1102 * (A - 8.7);
        const double dw = 2.0 * kPi * kTransHz / fs;
        taps = (int) std::ceil ((A - 8.0) / (2.285 * dw)) + 1;
        taps |= 1;
        half = taps / 2;
        std::vector<std::vector<double>> lps (kNumBands + 1, std::vector<double> ((size_t) taps));
        for (int e = 0; e <= kNumBands; ++e)
        {
            const double fc = std::min (kEdges[e], 0.49 * fs) / fs;
            double sum = 0.0;
            for (int i = 0; i < taps; ++i)
            {
                const double m = i - half;
                const double s = i == half ? 2.0 * fc : std::sin (2.0 * kPi * fc * m) / (kPi * m);
                const double r = m / half;
                const double w = detail::besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / detail::besselI0 (beta);
                lps[(size_t) e][(size_t) i] = s * w;
                sum += s * w;
            }
            for (auto& v : lps[(size_t) e])
                v /= sum;
        }
        for (int b = 0; b < kNumBands; ++b)
        {
            bank[(size_t) b].resize ((size_t) taps);
            for (int i = 0; i < taps; ++i)
                bank[(size_t) b][(size_t) i] = (float) (lps[(size_t) b + 1][(size_t) i] - lps[(size_t) b][(size_t) i]);
        }
    }

    // LIVE: the same magnitude as STUDIO's composite FIR, made minimum phase (real cepstrum, Oppenheim & Schafer):
    // causal, no delay, the energy at the front. bamp[b][k] = the amplitude of band b's linear-phase FIR on an
    // mN-point grid; per hop log|H| -> cepstrum -> fold -> exp -> the filter, stored time-reversed for dot().
    void designMinPhase()
    {
        mN = 512;
        while (mN < 8 * taps)
            mN <<= 1;
        mfft.prepare (mN);
        const int H = mN / 2;
        bamp.assign ((size_t) (kNumBands * (H + 1)), 0.0f);
        cep.assign ((size_t) mN, 0.0f);
        mspec.assign ((size_t) mN + 2, 0.0f);
        hmin.assign ((size_t) mN, 0.0f);
        std::vector<float> z ((size_t) mN), Z ((size_t) mN + 2);
        for (int b = 0; b < kNumBands; ++b)
        {
            std::fill (z.begin(), z.end(), 0.0f);
            for (int i = 0; i < taps; ++i)
                z[(size_t) ((i - half + mN) % mN)] = bank[(size_t) b][(size_t) i]; // zero phase: the centre at 0
            mfft.forward (z.data(), Z.data());
            for (int k = 0; k <= H; ++k)
                bamp[(size_t) (b * (H + 1) + k)] = Z[(size_t) (2 * k)];
        }
        taper.assign ((size_t) taps, 1.0f);
        const int nt = std::max (1, taps / 6);
        for (int i = 0; i < nt; ++i)
            taper[(size_t) (taps - nt + i)] = (float) (0.5 + 0.5 * std::cos (kPi * (i + 0.5) / nt));
    }

    void buildMinPhase() noexcept
    {
        const int H = mN / 2;
        for (int k = 0; k <= H; ++k)
        {
            float a = 1.0f;
            for (int b = 0; b < kNumBands; ++b)
                a += (gainLin[(size_t) b] - 1.0f) * bamp[(size_t) (b * (H + 1) + k)];
            mspec[(size_t) (2 * k)] = std::log (std::max (a, 1.0e-5f));
            mspec[(size_t) (2 * k + 1)] = 0.0f;
        }
        mfft.inverse (mspec.data(), cep.data());          // real cepstrum (even)
        for (int n = 1; n < H; ++n)                        // fold onto n >= 0: the minimum-phase cepstrum
            cep[(size_t) n] *= 2.0f;
        std::fill (cep.begin() + H + 1, cep.end(), 0.0f);
        mfft.forward (cep.data(), mspec.data());           // log H_min
        for (int k = 0; k <= H; ++k)
        {
            const float m = std::exp (mspec[(size_t) (2 * k)]), ph = mspec[(size_t) (2 * k + 1)];
            mspec[(size_t) (2 * k)] = m * std::cos (ph);
            mspec[(size_t) (2 * k + 1)] = m * std::sin (ph);
        }
        mfft.inverse (mspec.data(), hmin.data());
        for (int i = 0; i < taps; ++i)
            hNew[(size_t) (taps - 1 - i)] = hmin[(size_t) i] * taper[(size_t) i];
    }

    void update (const DetFrame& f) noexcept
    {
        // voice-body reference: the level of the voice's loud, voiced frames (tracks verse / chorus). Only clearly
        // periodic frames count (no noise floor, hum or breath can set it); their running 90th percentile gates out the
        // quiet ones. The reference is their peak level, released in 400 ms. Nothing is cut until it exists.
        double tot = 0.0;
        for (int b = 0; b < kNumBands; ++b)
            tot += f.E[b];
        periodicRun = (f.apRaw < 0.25f && f.bodyDb > -100.0f) ? periodicRun + 1 : 0;
        const bool voiced = periodicRun >= 3 && f.p < 0.3f; // 6 ms of periodicity: noise never manages that
        if (voiced)
        {
            if (! bodyQInit)
            {
                bodyQ = f.bodyDb;
                bodyQInit = true;
            }
            else
                bodyQ += 0.5f * (f.bodyDb > bodyQ ? 0.9f : -0.1f);
            if (f.bodyDb > bodyQ - 20.0f)
            {
                if (! bodyInit)
                {
                    bodyRef = f.bodyDb;
                    bodyInit = true;
                }
                else
                    bodyRef = f.bodyDb > bodyRef ? f.bodyDb : bodyRef + refK * (f.bodyDb - bodyRef);
            }
        }
        else if (bodyInit && f.bodyDb > bodyRef && f.p < 0.3f && tot < std::pow (10.0, f.bodyDb / 10.0))
            bodyRef = f.bodyDb; // a vowel swelling before its pitch settles: body-dominated, never a sibilant or noise
        float g[kNumBands] {};
        const float amount = amountToDb (params.amountPct);
        disp.hfReBodyDb = bodyInit ? (float) (10.0 * std::log10 (tot)) - bodyRef : -60.0f;
        disp.targetDb = amount;
        if (params.amountPct > 0.0f && bodyInit)
        {
            const double target = std::pow (10.0, (bodyRef + amount) / 10.0);
            bool act[kNumBands];
            double eAct = 0.0;
            for (int b = 0; b < kNumBands; ++b)
            {
                act[b] = params.focus == 0 || (params.focus == 1 ? b >= kFocusSplit : b < kFocusSplit);
                if (act[b])
                    eAct += f.E[b];
            }
            const double tAct = target - (tot - eAct);
            if (tot > target && eAct > 0.0)
            {
                // water-filling over the active bands: cap C with sum(min(E, C)) = tAct
                std::array<double, kNumBands> e {};
                int m = 0;
                for (int b = 0; b < kNumBands; ++b)
                    if (act[b])
                        e[(size_t) m++] = f.E[b];
                std::sort (e.begin(), e.begin() + m, [] (double a, double b2) { return a > b2; });
                double C = 1.0e-30, rest = eAct;
                if (tAct > 0.0)
                    for (int k = 0; k < m; ++k)
                    {
                        rest -= e[(size_t) k];                       // energy of the bands below the k+1 largest
                        const double c = (tAct - rest) / (k + 1);
                        if (c >= (k + 1 < m ? e[(size_t) k + 1] : 0.0))
                        {
                            C = std::max (c, 1.0e-30);
                            break;
                        }
                    }
                // weight: the detector's confidence (p^2: no cut where it is unsure). A level-only "safety net" on top
                // (cut any HF far above the voice body) was measured and rejected: 0.1-0.5 % fewer harsh frames for up
                // to 30x more change on vowels (research/bench_cpp.py, DECISIONS V4).
                const float w = f.p * f.p;
                for (int b = 0; b < kNumBands; ++b)
                    if (act[b] && f.E[b] > C)
                        g[b] = std::max ((float) (10.0 * std::log10 (C / f.E[b])), -params.rangeDb) * w;
            }
        }
        // look-ahead: min over the next kLook frames, then instant attack / 12 ms release (LIVE: no look-ahead)
        auto& slot = look[(size_t) lookPos];
        for (int b = 0; b < kNumBands; ++b)
            slot[(size_t) b] = g[b];
        pLook[(size_t) lookPos] = f.p;
        lookPos = (lookPos + 1) % (kLook + 1);
        const float pNow = live ? f.p : pLook[(size_t) lookPos]; // the oldest = frame being applied now
        bool identity = true;
        double eBefore = 0.0, eAfter = 0.0;
        for (int b = 0; b < kNumBands; ++b)
        {
            float mn = 0.0f;
            if (live)
                mn = g[b];
            else
                for (const auto& q : look)
                    mn = std::min (mn, q[(size_t) b]);
            float& s = smoothDb[(size_t) b];
            s = mn < s ? mn : s + (mn - s) * relK;
            if (s > -1.0e-4f)
                s = 0.0f;
            identity = identity && s >= 0.0f; // s <= 0 always: no reduction
            disp.bandGrDb[b] = -s;
            gainLin[(size_t) b] = std::pow (10.0f, s / 20.0f);
            eBefore += f.E[b];
            eAfter += f.E[b] * gainLin[(size_t) b] * gainLin[(size_t) b];
        }
        disp.grDb = eBefore > 0.0 ? (float) (-10.0 * std::log10 (std::max (eAfter, 1.0e-30) / eBefore)) : 0.0f;
        pPrev = sibilance();
        pApplied = pNow;
        disp.p = pNow;
        if (live)
        {
            std::swap (hOld, hNew);
            oldIdentity = newIdentity;
            newIdentity = identity;
            if (! identity)
                buildMinPhase();
            ramp = 0;
            return;
        }
        // new composite FIR for the next hop
        std::swap (hOld, hNew);
        oldIdentity = newIdentity;
        newIdentity = identity;
        if (! identity)
        {
            std::fill (hNew.begin(), hNew.end(), 0.0f);
            for (int b = 0; b < kNumBands; ++b)
            {
                const float a = gainLin[(size_t) b] - 1.0f;
                if (std::abs (a) > 0.0f)
                {
                    const float* hb = bank[(size_t) b].data();
                    for (int i = 0; i < taps; ++i)
                        hNew[(size_t) i] += a * hb[i];
                }
            }
        }
        ramp = 0;
    }

    double fs = 48000.0;
    int numCh = 2, hop = 96, delay = 0, taps = 1, half = 0, ringSize = 1, wpos = 0, ramp = 0;
    SibilanceDetector det;
    DeEssParams params;
    std::array<std::vector<float>, kNumBands> bank;
    std::array<std::vector<float>, kMaxCh> ring;
    std::vector<float> hOld, hNew;
    bool oldIdentity = true, newIdentity = true, newFrame = false;
    std::array<std::array<float, kNumBands>, kLook + 1> look {};
    std::array<float, kLook + 1> pLook {};
    int lookPos = 0, periodicRun = 0;
    std::array<float, kNumBands> smoothDb {}, gainLin {};
    float relK = 0.15f, refK = 0.007f, bodyRef = -140.0f, bodyQ = -140.0f, pApplied = 0.0f, pPrev = 0.0f;
    bool bodyInit = false, bodyQInit = false, live = false;
    int mN = 512;
    RealFft mfft;
    std::vector<float> bamp, cep, mspec, hmin, taper;
    DeEssDisplay disp;
};
} // namespace axv
