// Axell Velvet - DRIVE calibration: the added harmonics (above 40 Hz, against the model's own linear response) of a
// vocal at the nominal level, as a function of the drive in dB into each model. Prints the table Engine::kDriveTable
// (DRIVE 0..10 in steps of 0.5 -> dB) that makes every model add the same amount: -55 dB at DRIVE 0 rising 4 dB per
// step to -15 dB at DRIVE 10.   HeatCalibrate vocal.wav levelDb [startSeconds lengthSeconds]
#include "../Source/Engine.h"
#include "WavIO.h"
#include <cstdio>
#include <cstdlib>
using namespace axv;

// gDb: dB into the model above the nominal operating point, for a vocal recorded at LEVEL `level`
static std::vector<float> render (const std::vector<float>& x, double fs, int mode, float level, float gDb)
{
    Engine e;
    Params p;
    p.mode = mode;
    // the dB into the model is LEVEL + nominal offset + driveToDb (mode, drive): LEVEL is limited to +36 dB, so the
    // rest goes through DRIVE (run this with the linear placeholder table, 3.6 dB per step)
    const float want = level + gDb;
    const float lv = std::min (want, 30.0f);
    p.drive = (want - lv) / 3.6f;
    p.amountPct = 0.0f;
    p.guardPct = 0.0f;
    p.osLog2 = 4;
    p.levelDb = lv;
    if (std::abs (Engine::driveToDb (mode, p.drive) - 3.6f * p.drive) > 1.0e-3f || p.drive > 10.0f)
    {
        std::fprintf (stderr, "HeatCalibrate needs the linear placeholder table and gDb <= %.0f (drive %.3f -> %.4f)\n", 66.0 - level, p.drive, Engine::driveToDb (mode, p.drive));
        std::exit (1);
    }
    e.setParams (p);
    e.prepare (fs, 1, 512);
    const int lat = e.getLatencySamples();
    std::vector<float> b (x);
    b.resize (x.size() + (size_t) lat, 0.0f);
    for (size_t s = 0; s < b.size(); s += 512)
    {
        float* io[1] = { b.data() + s };
        e.process (io, 1, (int) std::min<size_t> (512, b.size() - s));
    }
    std::vector<float> y (b.begin() + lat, b.end());
    // 40 Hz high-pass (2 one-poles) for the measure
    const double k = std::exp (-2.0 * kPi * 40.0 / fs);
    double y1 = 0, x1 = 0, y2 = 0, x2 = 0;
    for (auto& s : y)
    {
        const double a = k * (y1 + s - x1); x1 = s; y1 = a;
        const double c = k * (y2 + a - x2); x2 = a; y2 = c;
        s = (float) c;
    }
    return y;
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: HeatCalibrate vocal.wav levelDb [start len]\n"); return 2; }
    wav::Audio a;
    if (! wav::read (argv[1], a)) return 1;
    const float level = (float) std::atof (argv[2]);
    const double st = argc > 3 ? std::atof (argv[3]) : 0.0, len = argc > 4 ? std::atof (argv[4]) : 30.0;
    const size_t i0 = std::min (a.frames(), (size_t) (st * a.fs)), i1 = std::min (a.frames(), i0 + (size_t) (len * a.fs));
    std::vector<float> x (a.ch[0].begin() + (long) i0, a.ch[0].begin() + (long) i1);
    std::printf ("// measured on %s, LEVEL %+.1f dB\n", argv[1], level);
    for (int mode = 0; mode < kNumModes; ++mode)
    {
        const auto lin = render (x, a.fs, mode, level, -40.0f);
        std::vector<std::pair<float, float>> curve; // (drive dB above nominal, harmonics dB)
        for (float g = -16.0f; g <= 48.0f; g += 1.0f)
        {
            // the linear reference was rendered 40 dB lower: same output scale (the pre/post gains cancel)
            const auto y = render (x, a.fs, mode, level, g);
            double e = 0, s = 0;
            for (size_t i = 0; i < y.size(); ++i) { const double d = y[i] - lin[i]; e += d * d; s += (double) lin[i] * lin[i]; }
            curve.push_back ({ g, (float) (10.0 * std::log10 (std::max (e, 1e-30) / s)) });
        }
        std::printf ("    { ");
        for (int k = 0; k <= 20; ++k)
        {
            const float target = -55.0f + 4.0f * 0.5f * (float) k;
            float g = curve.back().first;
            for (size_t i = 1; i < curve.size(); ++i)
                if (curve[i].second >= target)
                {
                    // the curve is monotone above the low-level floor; interpolate
                    const auto& p0 = curve[i - 1];
                    const auto& p1 = curve[i];
                    const float t = (target - p0.second) / std::max (1.0e-3f, p1.second - p0.second);
                    g = p0.first + std::clamp (t, 0.0f, 1.0f) * (p1.first - p0.first);
                    break;
                }
            std::printf ("%.2ff%s", g, k < 20 ? ", " : " },");
        }
        std::printf ("  // %s; floor %.1f dB at %.0f dB\n", mode == 0 ? "tape" : mode == 1 ? "console" : "tube", curve.front().second, curve.front().first);
    }
}
