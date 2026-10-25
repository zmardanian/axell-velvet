"""De-plosive prototype 2 (burst-gated): detection in the sub-80 Hz band (no voice lives there: a pop, a bump or a
voiced-stop burst is a sudden rise above that band's own floor), removal in 20-250 Hz for the burst only.
The band split is zero-phase here (the C++ code uses the equivalent linear-phase FIRs). usage: import"""
import numpy as np
from scipy import signal as sg
from scipy.ndimage import minimum_filter1d

def env(v, fs, ms):
    a = np.exp(-1.0 / (ms * 1e-3 * fs)); return np.sqrt(sg.lfilter([1 - a], [1, -a], v * v) + 1e-24)

def lowcut(x, fs, f=90):
    s = np.vstack([sg.butter(2, f, "high", fs=fs, output="sos")] * 1)
    hp1 = sg.butter(2, f, "high", fs=fs, output="sos")
    return sg.sosfilt(hp1, sg.sosfilt(hp1, x))   # 24 dB/oct like Velvet's LOW CUT (two 2nd-order sections)

def deplosive(x, fs, amount=100.0, thr=15.0, rise=8.0, look_ms=10.0, rel_ms=40.0, maxcut=24.0, k=0.8, detect_on=None, near=20.0):
    """x: the signal to clean (after LOW CUT); detect_on: the signal for detection (before LOW CUT: the sub-80 Hz
    band is where the pop shows); returns y and the gain curve (dB)"""
    d = x if detect_on is None else detect_on
    H = int(0.001 * fs)
    S = 20 * np.log10(env(sg.sosfilt(sg.butter(4, [20, 80], "band", fs=fs, output="sos"), d), fs, 4)[::H])
    Bd = 20 * np.log10(env(sg.sosfilt(sg.butter(2, [250, 4000], "band", fs=fs, output="sos"), d), fs, 10)[::H])
    bpk = np.empty(len(Bd)); pk = -120.0; kd = 1 - np.exp(-1.0 / 2000.0)
    for i in range(len(Bd)):
        pk = Bd[i] if Bd[i] > pk else pk + kd * (Bd[i] - pk); bpk[i] = pk
    n = len(S); fl = np.empty(n); f = S[0]
    up, dn = 0.002, 0.002 * 4                     # floor: 20 %-quantile tracker (2 dB/s up, 8 dB/s down)
    for i in range(n):
        f += up if S[i] > f else -dn; fl[i] = f
    ex = S - fl
    r = S - np.concatenate([np.full(10, S[0]), S[:-10]])
    g = np.zeros(n); burst = 0
    for i in range(n):
        if ex[i] > thr and S[i] > bpk[i] - near and (r[i] > rise or burst > 0):
            burst = 1
            g[i] = -min(maxcut, k * (ex[i] - thr + 6)) * amount / 100
        else:
            burst = 0
    L = int(look_ms); g = minimum_filter1d(g, 2 * L + 1, origin=-L)
    a = np.exp(-1.0 / rel_ms); s = 0.0
    for i in range(n):
        s = g[i] if g[i] < s else g[i] + a * (s - g[i]); g[i] = s
    G = np.interp(np.arange(len(x)), np.arange(n) * H, g)
    band = sg.sosfiltfilt(sg.butter(4, [20, 250], "band", fs=fs, output="sos"), x)
    return x - (1 - 10 ** (G / 20)) * band, G
