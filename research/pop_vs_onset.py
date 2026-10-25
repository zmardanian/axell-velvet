"""What LOW CUT 90 leaves of the artist's plosive pops (Velvet 2.0 de-plosive decision, DECISIONS V18).

For every take in the local manifest (takes.py), after LOW CUT 90: the 90-250 Hz level in the 120 ms after each pop
(deplosive_proto.pops) and after each ordinary loud word start (no pop within 200 ms), each against the same band in
the vowel that follows (150-400 ms later). If the pops stand no higher than ordinary word starts, a de-plosive has
nothing left to find that is not also a normal onset. usage: python3 pop_vs_onset.py"""
import os, sys, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diag_realtake as D, deplosive_proto as P, deplosive2 as P2, takes

def band(x, fs, lo, hi):
    return sg.sosfiltfilt(sg.butter(4, [lo, hi], "band", fs=fs, output="sos"), x)

def onsets(x, fs):
    """loud word starts: the 150 Hz - 4 kHz body rises > 12 dB in 100 ms to within 25 dB of the take's peak"""
    eb = 20 * np.log10(P.env(band(x, fs, 150, 4000), fs, 20) + 1e-12); top = np.percentile(eb, 99)
    H = int(0.01 * fs); e = eb[::H]; out = []
    for i in range(15, len(e) - 15):
        if e[i] > top - 25 and e[i] - e[i - 10] > 12 and (not out or i * H - out[-1] > 0.3 * fs):
            out.append(i * H)
    return out

if __name__ == "__main__":
    rows = {"pop": [], "onset": []}
    for f in takes.real_takes():
        fs, x = D.read(f)
        lb = band(P2.lowcut(x, fs, 90), fs, 90, 250); pops = P.pops(x, fs)
        n1, n2 = int(0.12 * fs), int(0.25 * fs)
        rel = lambda k: 10 * np.log10(np.mean(lb[k:k + n1] ** 2) / (np.mean(lb[k + int(0.15 * fs):k + int(0.15 * fs) + n2] ** 2) + 1e-20) + 1e-20)
        rows["pop"] += [rel(k) for k in pops]
        rows["onset"] += [rel(k) for k in onsets(x, fs) if all(abs(k - p) > 0.2 * fs for p in pops)]
    for k, v in rows.items():
        v = np.array(v)
        print(f"{k:6s} n={len(v):4d}  90-250 Hz, first 120 ms vs the vowel after it: median {np.median(v):+.1f} dB, "
              f"p90 {np.percentile(v, 90):+.1f}, max {v.max():+.1f}")
