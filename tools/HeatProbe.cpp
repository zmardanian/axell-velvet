// Axell Velvet - HEAT model probe: harmonic levels (H2..H5, THD) vs drive for each model at a given rate.
#include "../Source/Heat.h"
#include <cstdio>
#include <complex>
#include <vector>
using namespace axv;
template <typename M>
void probe (const char* name, double fs, double f0)
{
    std::printf ("%s @ %.0f Hz, %.0f Hz sine\n", name, fs, f0);
    for (double dB : { -24.0, -18.0, -12.0, -6.0, 0.0, 6.0 })
    {
        M m;
        m.prepare (fs);
        const double amp = std::pow (10.0, dB / 20.0);
        const int per = (int) std::lround (fs / f0), n = per * 400;
        const double f = fs / per;
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) (amp * std::sin (2.0 * kPi * f * i / fs));
        m.process (x.data(), n, 0);
        // last 200 periods
        double h[8] {};
        for (int k = 1; k < 8; ++k)
        {
            std::complex<double> s = 0;
            for (int i = n - per * 200; i < n; ++i)
                s += (double) x[(size_t) i] * std::polar (1.0, -2.0 * kPi * k * f * i / fs);
            h[k] = 2.0 * std::abs (s) / (per * 200);
        }
        double thd = 0;
        for (int k = 2; k < 8; ++k)
            thd += h[k] * h[k];
        std::printf ("  in %+5.0f dB: gain %+6.2f dB  H2 %6.1f  H3 %6.1f  H4 %6.1f  H5 %6.1f dBc  THD %6.2f %%\n", dB, 20 * std::log10 (h[1] / amp),
                     20 * std::log10 (h[2] / h[1] + 1e-12), 20 * std::log10 (h[3] / h[1] + 1e-12), 20 * std::log10 (h[4] / h[1] + 1e-12),
                     20 * std::log10 (h[5] / h[1] + 1e-12), 100 * std::sqrt (thd) / h[1]);
    }
}
int main (int argc, char** argv)
{
    const double fs = argc > 1 ? std::atof (argv[1]) : 192000.0;
    for (double f0 : { 100.0, 1000.0, 5000.0 })
    {
        probe<TapeModel> ("TAPE", fs, f0);
        probe<ConsoleModel> ("CONSOLE", fs, f0);
        probe<TubeModel> ("TUBE", fs, f0);
    }
}
