// Axell Velvet - offline render through the complete engine (benchmarks, preset checks, listening files).
// VelvetRender in.wav out.wav [key=value ...]   keys: amount range focus listen deess heat mode drive guard tone mix
//                                                    level output os lowcut block
// Output is latency-compensated (same length and alignment as the input), float32.
#include "../Source/Engine.h"
#include "WavIO.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: VelvetRender in.wav out.wav [key=value ...]\n";
        return 2;
    }
    wav::Audio a;
    if (! wav::read (argv[1], a))
    {
        std::cerr << "cannot read " << argv[1] << "\n";
        return 1;
    }
    axv::Params p;
    int block = 512;
    for (int i = 3; i < argc; ++i)
    {
        std::string kv = argv[i];
        const auto eq = kv.find ('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = kv.substr (0, eq);
        const float v = (float) std::atof (kv.c_str() + eq + 1);
        if (k == "amount") p.amountPct = v;
        else if (k == "range") p.rangeDb = v;
        else if (k == "focus") p.focus = (int) v;
        else if (k == "listen") p.listen = v != 0;
        else if (k == "deess") p.deessOn = v != 0;
        else if (k == "heat") p.heatOn = v != 0;
        else if (k == "mode") p.mode = (int) v;
        else if (k == "drive") p.drive = v;
        else if (k == "guard") p.guardPct = v;
        else if (k == "tone") p.tone = v;
        else if (k == "mix") p.mixPct = v;
        else if (k == "level") p.levelDb = v;
        else if (k == "output") p.outputDb = v;
        else if (k == "os") p.osLog2 = (int) v;
        else if (k == "lowcut") p.lowCut = (int) v;
        else if (k == "block") block = (int) v;
        else std::cerr << "unknown key " << k << "\n";
    }
    const int nch = std::min<int> (2, (int) a.ch.size());
    axv::Engine e;
    e.setParams (p);
    e.prepare (a.fs, nch, block);
    const int lat = e.getLatencySamples();
    const size_t n = a.frames(), total = n + (size_t) lat;
    std::vector<std::vector<float>> buf ((size_t) nch, std::vector<float> (total, 0.0f));
    for (int c = 0; c < nch; ++c)
        std::copy (a.ch[(size_t) c].begin(), a.ch[(size_t) c].end(), buf[(size_t) c].begin());
    const auto t0 = std::chrono::steady_clock::now();
    axv::DisplayFrame f;
    for (size_t s = 0; s < total; s += (size_t) block)
    {
        const int len = (int) std::min<size_t> ((size_t) block, total - s);
        float* io[2] = { buf[0].data() + s, nch > 1 ? buf[1].data() + s : nullptr };
        e.process (io, nch, len);
        while (e.popDisplay (f)) {}
    }
    const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    wav::Audio o;
    o.fs = a.fs;
    o.ch.resize ((size_t) nch);
    for (int c = 0; c < nch; ++c)
        o.ch[(size_t) c].assign (buf[(size_t) c].begin() + lat, buf[(size_t) c].end());
    std::cout << "latency " << lat << " samples (" << 1000.0 * lat / a.fs << " ms); " << 100.0 * sec / ((double) n / a.fs)
              << " % of real time\n";
    return wav::write (argv[2], o) ? 0 : 1;
}
