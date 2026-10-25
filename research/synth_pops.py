"""Ground truth for the de-plosive: clean CMU ARCTIC speech (studio, no pops) with plosive pops added at the labelled
p / b releases. A pop is the capsule's response to a puff of air: a low-frequency pressure pulse (gamma pulse,
time constant 4-14 ms) plus a little turbulence (noise band-limited to 40-300 Hz), lasting 50-110 ms as on the artist's
takes; its 90-250 Hz share varies (weak pops are sub-90 Hz only, strong ones reach 250 Hz). Level: the pop's 20-250 Hz
energy over the voice's own 20-250 Hz energy in the 150 ms after the release, +5 to +35 dB.

Scores for a processed take y against the clean voice v (both 48 kHz, aligned), in the 20-250 Hz band:
  pop left      error energy in the pop windows after processing, re before (dB; lower = more removed)
  onset change  |change| of the voice at the word starts with a pop (the vowel under the pop: dB, lower = better)
  elsewhere     |change| of the voice's 20-250 Hz everywhere else (thinning: dB, lower = better)
usage: import; examples(n) -> list of (x, v, pop_spans)"""
import os, sys, zlib, numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from data import utterances, load

FS = 48000

def pop(rng, fs=FS):
    tau = rng.uniform(0.004, 0.014); n = int(rng.uniform(0.05, 0.11) * fs); t = np.arange(n) / fs
    p = (t / tau) * np.exp(1 - t / tau)                                   # pressure pulse
    turb = sg.sosfilt(sg.butter(2, [40, 300], "band", fs=fs, output="sos"), rng.standard_normal(n)) * np.exp(-t / (2 * tau))
    p = p + rng.uniform(0.05, 0.4) * turb / (np.std(turb) + 1e-12) * np.max(p) * 0.3
    hi = sg.sosfilt(sg.butter(2, [90, 250], "band", fs=fs, output="sos"), p)   # tilt the 90-250 Hz share
    p = p + rng.uniform(-0.8, 2.5) * hi
    p *= np.hanning(2 * n)[n:] ** 0.5                                      # die away
    return p * rng.choice([-1, 1])

def lowband(v, fs=FS):
    return sg.sosfiltfilt(sg.butter(4, [20, 250], "band", fs=fs, output="sos"), v)

def examples(n_utt=60, seed=0, speakers=("slt", "bdl", "jmk"), start=600):
    out = []
    for spk in speakers:
        for name, w, l in list(utterances(spk))[start:start + n_utt // len(speakers)]:
            rng = np.random.default_rng(zlib.crc32(f"pop/{spk}/{name}/{seed}".encode()))
            fs0, x, _, ph = load(spk, w, l)
            v = sg.resample_poly(x, 3, 2); v = v / (np.max(np.abs(v)) + 1e-9) * 10 ** (rng.uniform(-20, -6) / 20)
            lv = lowband(v); x = v.copy(); spans = []
            for a, b, q in ph:
                if q in ("p", "b") and rng.random() < 0.85:
                    r = int(b * FS)                                         # release = end of the closure label
                    pp = pop(rng); m = min(len(pp), len(x) - r)
                    if m < 1000:
                        continue
                    ref = np.sum(lv[r:r + int(0.15 * FS)] ** 2) + 1e-12
                    lp = lowband(np.concatenate([np.zeros(r), pp[:m], np.zeros(len(x) - r - m)]))[r:r + m]
                    g = np.sqrt(ref * 10 ** (rng.uniform(5, 35) / 10) / (np.sum(lp ** 2) + 1e-20))
                    x[r:r + m] += g * pp[:m]; spans.append((r, r + m))
            if spans:
                out.append((x, v, spans))
    return out

def score(y, x, v, spans):
    """pop error left (dB, in the pop windows, after vs before) and thinning: mean |change| of the voice's 20-250 Hz
    level over 30 ms frames away from the pops (+-200 ms), voiced frames only (within 30 dB of the take's loudest)"""
    ly, lx, lvv = lowband(y), lowband(x), lowband(v)
    m = np.zeros(len(v), bool); away = np.ones(len(v), bool)
    for a, b in spans:
        m[max(0, a - 480):b + 2400] = True; away[max(0, a - int(0.2 * FS)):b + int(0.2 * FS)] = False
    before = np.sum((lx - lvv)[m] ** 2); after = np.sum((ly - lvv)[m] ** 2)
    N = int(0.03 * FS); fr = range(0, len(v) - N, N)
    ev = np.array([10 * np.log10(np.mean(lvv[i:i + N] ** 2) + 1e-20) for i in fr])
    ey = np.array([10 * np.log10(np.mean(ly[i:i + N] ** 2) + 1e-20) for i in fr])
    aw = np.array([away[i:i + N].all() for i in fr]) & (ev > ev.max() - 30)
    return dict(pop_left=10 * np.log10(after / (before + 1e-20) + 1e-20), thin=float(np.mean(np.abs(ey[aw] - ev[aw]))) if aw.any() else 0.0,
                n=len(spans))
