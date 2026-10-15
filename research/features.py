"""Frame features for sibilance detection (STFT prototype; the plugin computes the same quantities with filters).
32 kHz, 16 ms Hann window, 2 ms hop."""
import numpy as np
from scipy import signal as sg
WIN, HOP = 512, 64

def stft_power(x, fs):
    f, t, Z = sg.stft(x, fs, window="hann", nperseg=WIN, noverlap=WIN - HOP, boundary=None, padded=False)
    return f, t, np.abs(Z) ** 2

def band(P, f, lo, hi):
    m = (f >= lo) & (f < hi)
    return P[m].sum(0) + 1e-12, m

def features(x, fs):
    f, t, P = stft_power(x, fs)
    body, _ = band(P, f, 100, 3000)
    sib, ms = band(P, f, 3500, 14000)
    low_sib, _ = band(P, f, 2000, 3500)
    total = P.sum(0) + 1e-12
    hf = P[ms] + 1e-15
    flat = np.exp(np.mean(np.log(hf), 0)) / np.mean(hf, 0)           # spectral flatness of the sibilance band
    cen = (f[ms][:, None] * P[ms]).sum(0) / sib                       # sibilance-band centroid
    # voicing: normalised autocorrelation peak (pitch 70-400 Hz) of the < 1 kHz signal, per frame
    lp = sg.sosfilt(sg.butter(4, 1000, fs=fs, output="sos"), x)
    n = P.shape[1]
    voi = np.zeros(n)
    L = WIN
    for i in range(n):
        s = lp[i * HOP:i * HOP + L]
        if len(s) < L or np.dot(s, s) < 1e-9:
            continue
        ac = np.correlate(s, s, "full")[L - 1:]
        lo, hi = int(fs / 400), int(fs / 70)
        voi[i] = ac[lo:hi].max() / ac[0]
    # level relative to the utterance's loud (voiced) level: excludes silence / breath
    lvl = 10 * np.log10(total)
    ref = np.percentile(lvl, 95)
    F = np.stack([10 * np.log10(sib / body), flat, cen / 1000.0, voi, lvl - ref, 10 * np.log10(sib / low_sib), 10 * np.log10(sib) - ref], 1)
    return t, F

NAMES = ["sib/body dB", "HF flatness", "HF centroid kHz", "voicing", "level re loud", "sib/2-3.5k dB", "HF level re loud"]
