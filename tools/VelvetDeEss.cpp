// Axell Velvet - offline de-esser render (research / benchmarks): VelvetDeEss in.wav out.wav [amount] [range] [focus] [listen]
// Output is latency-compensated (same length and alignment as the input). Optional 7th arg: per-frame CSV (t, p, grDb);
// --live anywhere: the LIVE de-esser (zero latency).
#include "../Source/Sibilance.h"
#include "WavIO.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: VelvetDeEss in.wav out.wav [amount%] [rangeDb] [focus] [listen] [frames.csv]\n";
        return 2;
    }
    wav::Audio a;
    if (! wav::read (argv[1], a))
    {
        std::cerr << "cannot read " << argv[1] << "\n";
        return 1;
    }
    axv::DeEssParams p;
    if (argc > 3) p.amountPct = (float) std::atof (argv[3]);
    if (argc > 4) p.rangeDb = (float) std::atof (argv[4]);
    if (argc > 5) p.focus = std::atoi (argv[5]);
    if (argc > 6) p.listen = std::atoi (argv[6]) != 0;
    FILE* csv = argc > 7 && argv[7][0] != '-' ? std::fopen (argv[7], "w") : nullptr;
    const int nch = std::min<int> (2, (int) a.ch.size());
    bool live = false;
    for (int i = 3; i < argc; ++i)
        live = live || std::string (argv[i]) == "--live";
    axv::DeEsser d;
    d.prepare (a.fs, nch, live ? axv::DetectorConfig::live() : axv::DetectorConfig {});
    d.setParams (p);
    const int lat = d.latency();
    const size_t n = a.frames(), total = n + (size_t) lat;
    std::vector<std::vector<float>> buf ((size_t) nch, std::vector<float> (total, 0.0f));
    for (int c = 0; c < nch; ++c)
        std::copy (a.ch[(size_t) c].begin(), a.ch[(size_t) c].end(), buf[(size_t) c].begin());
    const int block = 256;
    for (size_t s = 0; s < total; s += block)
    {
        const int len = (int) std::min<size_t> (block, total - s);
        float* io[2] = { buf[0].data() + s, nch > 1 ? buf[1].data() + s : nullptr };
        d.process (io, nch, len);
        if (csv && d.frameReady())
        {
            const auto& f = d.detector().out();
            std::fprintf (csv, "%.5f,%.4f,%.3f,%.2f\n", (double) f.center / a.fs, d.display().p, d.display().grDb, d.display().hfReBodyDb);
        }
    }
    wav::Audio o;
    o.fs = a.fs;
    o.ch.resize ((size_t) nch);
    for (int c = 0; c < nch; ++c)
        o.ch[(size_t) c].assign (buf[(size_t) c].begin() + lat, buf[(size_t) c].end());
    if (csv) std::fclose (csv);
    std::cout << "latency " << lat << " samples (" << 1000.0 * lat / a.fs << " ms), FIR " << d.firTaps() << " taps\n";
    return wav::write (argv[2], o) ? 0 : 1;
}
