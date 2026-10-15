#pragma once
// Axell core - real FFT (power-of-two sizes), JUCE-free. Iterative radix-2 complex FFT of size N/2 with precomputed
// twiddles and bit-reversal table, plus the standard real-signal split. prepare() allocates; forward() does not.
// Output: bins 0..N/2 as interleaved (re, im) in a float array of size N + 2.
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace axell
{
class RealFft
{
public:
    void prepare (int size)
    {
        n = size;
        h = n / 2;
        int bits = 0;
        while ((1 << bits) < h)
            ++bits;
        rev.assign ((size_t) h, 0);
        for (int i = 0; i < h; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        tw.resize ((size_t) (h / 2 > 0 ? h / 2 : 1));
        for (int k = 0; k < h / 2; ++k)
            tw[(size_t) k] = std::polar (1.0, -2.0 * kPi * k / h);
        split.resize ((size_t) h);
        for (int k = 0; k < h; ++k)
            split[(size_t) k] = std::polar (1.0, -2.0 * kPi * k / n);
        z.assign ((size_t) h, {});
    }

    int size() const noexcept { return n; }

    // in: n real samples; out: n + 2 floats (n/2 + 1 complex bins, unnormalised: X[k] = sum x[t] e^{-2 pi i k t / n})
    void forward (const float* in, float* out) noexcept
    {
        for (int i = 0; i < h; ++i)
            z[(size_t) rev[(size_t) i]] = { (double) in[2 * i], (double) in[2 * i + 1] };
        for (int len = 2; len <= h; len <<= 1)
        {
            const int half = len / 2, step = h / len;
            for (int s = 0; s < h; s += len)
                for (int k = 0; k < half; ++k)
                {
                    const auto t = tw[(size_t) (k * step)] * z[(size_t) (s + k + half)];
                    const auto u = z[(size_t) (s + k)];
                    z[(size_t) (s + k)] = u + t;
                    z[(size_t) (s + k + half)] = u - t;
                }
        }
        // split: X[k] = (Z[k] + conj Z[h-k]) / 2 - i W^k (Z[k] - conj Z[h-k]) / 2
        for (int k = 0; k <= h; ++k)
        {
            const auto a = z[(size_t) (k % h)];
            const auto b = std::conj (z[(size_t) ((h - k) % h)]);
            const auto e = 0.5 * (a + b), o = std::complex<double> (0.0, -0.5) * (a - b);
            const auto w = k < h ? split[(size_t) k] : std::complex<double> (-1.0, 0.0);
            const auto x = e + w * o;
            out[2 * k] = (float) x.real();
            out[2 * k + 1] = (float) x.imag();
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    int n = 0, h = 0;
    std::vector<int> rev;
    std::vector<std::complex<double>> tw, split, z;
};
} // namespace axell
