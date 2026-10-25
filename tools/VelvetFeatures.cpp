// Axell Velvet - detector feature dump for training (research/train_cpp.py): runs the shipping SibilanceDetector on
// raw float32 mono files. Manifest lines: <in.f32> <sampleRate> <out.f32>; --live: the LIVE (causal) detector.
// Output rows of kCols float32:
//   centre (s), feat[10], adapted[10], x[14], E[10], bodyDb, levelDb, f0, apRaw, p
#include "../Source/Sibilance.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: VelvetFeatures manifest.txt [--live]\n";
        return 2;
    }
    std::ifstream man (argv[1]);
    const bool live = argc > 2 && std::string (argv[2]) == "--live";
    std::string line;
    int files = 0;
    while (std::getline (man, line))
    {
        std::istringstream ss (line);
        std::string in, out;
        double fs = 0;
        if (! (ss >> in >> fs >> out))
            continue;
        std::ifstream f (in, std::ios::binary | std::ios::ate);
        const auto bytes = (size_t) f.tellg();
        std::vector<float> x (bytes / sizeof (float));
        f.seekg (0);
        f.read ((char*) x.data(), (std::streamsize) (x.size() * sizeof (float)));
        axv::SibilanceDetector det;
        det.prepare (fs, live ? axv::DetectorConfig::live() : axv::DetectorConfig {});
        std::vector<float> rows;
        for (float v : x)
            if (det.push (v))
            {
                const auto& o = det.out();
                rows.push_back ((float) ((double) o.center / fs));
                rows.insert (rows.end(), o.feat, o.feat + axv::kNumFeat);
                rows.insert (rows.end(), o.adapted, o.adapted + axv::kNumFeat);
                rows.insert (rows.end(), o.x, o.x + axv::kNumDesign);
                rows.insert (rows.end(), o.E, o.E + axv::kNumBands);
                rows.push_back (o.bodyDb);
                rows.push_back (o.levelDb);
                rows.push_back (o.f0);
                rows.push_back (o.apRaw);
                rows.push_back (o.p);
            }
        std::ofstream fo (out, std::ios::binary);
        fo.write ((const char*) rows.data(), (std::streamsize) (rows.size() * sizeof (float)));
        ++files;
    }
    std::cout << files << " files\n";
    return 0;
}
