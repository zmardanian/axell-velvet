"""Detector features v2 (32 kHz, 16 ms window, 2 ms hop): YIN aperiodicity, per-band spectral shape relative to the voice
body, HF flatness, level. All are quantities the plugin can compute with filters + a pitch tracker."""
import numpy as np
from scipy import signal as sg
from yin import yin
WIN, HOP = 512, 64
BANDS = [(1500, 2500), (2500, 3500), (3500, 5000), (5000, 7000), (7000, 10000), (10000, 14000)]
NAMES = ["aperiodicity", "HF flatness", "level re loud"] + [f"{a/1000:g}-{b/1000:g}k re body" for a, b in BANDS] + ["HF total re body"]

def features(x, fs):
    f, t, Z = sg.stft(x, fs, window="hann", nperseg=WIN, noverlap=WIN - HOP, boundary=None, padded=False)
    P = np.abs(Z) ** 2
    ap, f0 = yin(x, fs, HOP)
    n = min(P.shape[1], len(ap)); P = P[:, :n]; t = t[:n]; ap = ap[:n]
    def band(lo, hi): m = (f >= lo) & (f < hi); return P[m].sum(0) + 1e-14
    body = band(100, 1500)
    m = (f >= 2500) & (f < 14000)
    flat = np.exp(np.mean(np.log(P[m] + 1e-16), 0)) / np.mean(P[m] + 1e-16, 0)
    lvl = 10 * np.log10(P.sum(0) + 1e-14); ref = np.percentile(lvl, 95)
    cols = [ap, flat, lvl - ref] + [10 * np.log10(band(a, b) / body) for a, b in BANDS] + [10 * np.log10(band(1500, 14000) / body)]
    return t, np.stack(cols, 1)
